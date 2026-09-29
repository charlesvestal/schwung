#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A TURN THAT OPENS A LIST MUST NOT BECOME A DECISION.
#
# Turning the grid's LFO Target cell or a file cell opens a flat list, and a
# release (or 1 s idle) commits the row under the cursor. So wherever the
# cursor LANDS is what a one-detent brush writes:
#
#   - LFO Target opened on "None" when the target read failed (null), when a
#     component section read timed out, or when the route was not listed --
#     and the release wrote target="" + enabled=0: a working LFO switched off.
#   - A file cell opened on files[0] when the read failed, the value was
#     empty, or the file sat outside the root -- and the release LOADED it.
#
# Now: a null read refuses to open; an unlisted routing gets its own row; and
# neither list writes anything unless the cursor was moved. A refused open is
# latched for the rest of the spin (no read, no scan, no speech per detent).
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { buildFlatTargetRows, planFlatTargetOpen, flatTargetCommitRow, moveFlatCursor, fitDividerLabel }
  from "./src/shared/lfo_target_flat.mjs";
import { planFileFlatOpen, fileFlatPick, createRefusalLatch } from "./src/shared/file_flat.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };
const P = (...keys) => keys.map((k) => ({ key: k, label: k.toUpperCase() }));
const comps = [{ key: "synth", label: "Synth: Mini-JV" }, { key: "fx1", label: "FX 1: Freeverb" }];
const rows = buildFlatTargetRows(comps, (c) => [[{ label: null, params: P("cut", "res") }],
                                              [{ label: null, params: P("mix") }]][c]);

/* 1. A failed read refuses to open. */
if (planFlatTargetOpen(rows, null, "cut") !== null) bad("null target read must refuse");
if (planFlatTargetOpen(rows, "synth", null) !== null) bad("null target_param read must refuse");

/* 2. No routing: None; listed routing: its row. */
{
  const p = planFlatTargetOpen(rows, "", "");
  if (!p || p.stored !== 0) bad("an empty routing is None");
  const q = planFlatTargetOpen(rows, "fx1", "mix");
  if (!q || q.rows[q.stored].label !== "MIX") bad("a listed routing opens on its row");
}

/* 3. An UNLISTED routing (section read timed out, or not offered) gets a row
 *    of its own, and the cursor sits on it -- never on None. */
{
  const missing = buildFlatTargetRows(comps, (c) => (c === 0 ? [] : [{ label: null, params: P("mix") }]));
  const p = planFlatTargetOpen(missing, "synth", "cut");
  if (!p) bad("unlisted routing must still open");
  else {
    const r = p.rows[p.stored];
    if (p.stored === 0) bad("unlisted routing opened on None");
    if (!r.route || r.route.target !== "synth" || r.route.param !== "cut") bad("the added row holds the stored route");
    if (p.rows[0].label !== "None") bad("None still heads the list");
    if (moveFlatCursor(p.rows, 0, 1) !== p.stored) bad("the added row is selectable right after None");
    /* Brushing and releasing writes nothing. */
    if (flatTargetCommitRow(p.rows, p.stored, p.stored) !== null) bad("an unmoved release must not commit");
  }
}

/* 4. Commit only a MOVED cursor. */
{
  const p = planFlatTargetOpen(rows, "synth", "res");
  if (flatTargetCommitRow(p.rows, p.stored, p.stored) !== null) bad("the stored row is not a choice (re-write resets the mod base)");
  const none = flatTargetCommitRow(p.rows, 0, p.stored);
  if (!none || none.route.target !== "") bad("moving to None is a real clear");
  const mix = flatTargetCommitRow(p.rows, p.rows.findIndex((r) => r.label === "MIX"), p.stored);
  if (!mix || mix.route.param !== "mix") bad("moving to another row commits it");
}

/* 5. File cell: refuse null, refuse empty, and a release that never moved
 *    loads nothing -- even when the loaded file is not in the list. */
{
  const files = [{ path: "/r/a.wav", label: "a" }, { path: "/r/b.wav", label: "b" }];
  if (planFileFlatOpen(null, files).refuse !== "unreadable") bad("a null file read must refuse");
  if (planFileFlatOpen("/r/a.wav", []).refuse !== "empty") bad("an empty folder refuses");
  const on = planFileFlatOpen("/r/b.wav", files);
  if (on.stored !== 1 || on.index !== 1) bad("opens on the loaded file");
  const off = planFileFlatOpen("/elsewhere/x.wav", files);
  if (off.stored !== -1 || off.index !== 0) bad("an unlisted file opens on row 0, stored -1");
  if (fileFlatPick(files, off.index, off.stored, false) !== null) bad("unmoved release after an unlisted open LOADED files[0]");
  const empty = planFileFlatOpen("", files);
  if (fileFlatPick(files, empty.index, empty.stored, false) !== null) bad("unmoved release on an empty value loaded a file");
  if (fileFlatPick(files, 1, 1, true) !== null) bad("moving back onto the loaded file is not a change");
  const pick = fileFlatPick(files, 1, 0, true);
  if (!pick || pick.path !== "/r/b.wav") bad("a moved cursor loads its file");
}

/* 6. A refusal is latched for the rest of the spin, per key. */
{
  const l = createRefusalLatch(1000);
  if (l.latched("k", 0)) bad("nothing latched yet");
  l.refuse("k", 0);
  if (!l.latched("k", 500)) bad("same spin is latched");
  if (!l.latched("k", 1400)) bad("each detent extends the spin");
  if (l.latched("other", 1500)) bad("another key is not latched");
  if (l.latched("k", 5000)) bad("a later spin asks again");
}

/* 7. A level-1 caption is fitted 10 px narrower (drawMenuList indents it). */
{
  const m = (t) => t.length * 6;
  const r = buildFlatTargetRows([{ key: "synth", label: "Synth: X" }],
    () => [{ label: "ABCDEFGHIJ", params: P("p") }], { measure: m, maxW: 60 });
  const sec = r.find((x) => x.type === "divider" && x.level === 1);
  if (!sec || m(sec.label) + 10 > 60) bad("level-1 caption overruns the gutter: " + (sec && sec.label));
}

/* 8. The REAL host functions, lifted out of shadow_ui.js and driven. */
{
  const src = fs.readFileSync("./src/shadow/shadow_ui.js", "utf8");
  const body = (name) => {
    const at = src.indexOf("function " + name + "(");
    const end = at >= 0 ? src.indexOf("\n}\n", at) : -1;
    if (end < 0) { console.log("FAIL: " + name + " is gone from shadow_ui.js"); process.exit(1); }
    return src.slice(at, end + 2);
  };
  const world = (store) => {
    const log = { views: [], said: [], commits: [], writes: [], reads: 0 };
    const deps = {
      VIEWS: { LFO_TARGET_FLAT: "flat", LFO_EDIT: "lfoedit", FILE_FLAT: "fileflat", PARAM_PAGES: "pp" },
      MASTER_SETTINGS_COMPONENT: "master_settings",
      paramPagesComponent: () => "slot",
      makeSlotLfoCtx: () => ({ getParam: (k) => { log.reads++; return store[k]; } }),
      makeMfxLfoCtx: () => null,
      flatRefusal: createRefusalLatch(1000),
      announce: (t) => log.said.push(t),
      announceMenuItem: (t) => log.said.push(t),
      clearParamPagesTouch: () => {}, enterLfoTargetPicker: () => {},
      listKnobInit: () => ({}),
      buildFlatTargetRows, planFlatTargetOpen, flatTargetCommitRow,
      lfoTargetComponents: comps,
      /* the synth section read TIMED OUT: it offers nothing */
      lfoTargetSectionsOf: (c) => (c === 0 ? [] : [{ label: null, params: P("mix") }]),
      text_width: (t) => t.length * 6, SCREEN_WIDTH: 128, LIST_LABEL_X: 4,
      setView: (v) => log.views.push(v),
      lfoTargetFlatAnnounce: () => {},
      commitLfoTargetFromGrid: (ctx, route) => log.commits.push(route),
      returnToSlotGridFromLfoTarget: () => true,
      paramPagesActive: () => true,
      getSlotParam: () => { log.reads++; return store.file; },
      buildFilepathBrowserState: () => ({ currentDir: "/r/kits", items: store.items }),
      refreshFilepathBrowser: () => {}, FILEPATH_BROWSER_FS: null,
      planFileFlatOpen, fileFlatPick,
      commitParamPagesValue: (k, v) => log.writes.push(k + "=" + v),
      fileFlatAfterCommit: () => {},
    };
    const names = Object.keys(deps);
    const fns = ["lfoTargetKnobEnter", "lfoTargetFlatCommit", "fileFlatEnter", "fileFlatMove", "fileFlatCommit", "fileFlatClose"];
    const code = "let lfoCtx = null, lfoTargetFromGrid = false, lfoTargetKnob, lfoTargetFlatRows = [], " +
      "lfoTargetFlatIndex = 0, lfoTargetFlatStored = 0, lfoTargetKnobCommit = -1, lfoTargetKnobHeld = false, " +
      "lfoTargetKnobLastMs = 0, needsRedraw = false, fileFlatRows = [], fileFlatIndex = 0, fileFlatStored = -1, " +
      "fileFlatKey = \"\", fileFlatFolder = \"\", fileFlatKnobState, fileFlatKnob = -1, fileFlatHeld = false, " +
      "fileFlatLastMs = 0, fileFlatMoved = false, fileFlatSlot = -1, fileFlatFullKey = \"\", fileFlatMeta = null;\n" +
      fns.map(body).join("\n") +
      "\nreturn { " + fns.join(", ") + ", st: () => ({ lfoTargetFlatIndex, lfoTargetFlatStored, lfoTargetFlatRows }) };";
    const api = new Function(...names, code)(...names.map((n) => deps[n]));
    return { api, log };
  };

  /* LFO: a null read refuses -- no view, no commit -- and the spin stays quiet. */
  {
    const { api, log } = world({ target: null, target_param: "cut" });
    if (api.lfoTargetKnobEnter(0, "slot:lfo1:target", 1, 3, true) !== true) bad("refusal must consume the turn");
    if (log.views.length) bad("a null read opened the list: " + log.views);
    const reads = log.reads;
    api.lfoTargetKnobEnter(0, "slot:lfo1:target", 1, 3, true);
    if (log.reads !== reads || log.said.length !== 1) bad("a refused spin re-read or re-announced");
  }
  /* LFO: section read timed out, routing unlisted -> cursor on its own row,
   * and a release with no move writes NOTHING. */
  {
    const { api, log } = world({ target: "synth", target_param: "cut" });
    api.lfoTargetKnobEnter(0, "slot:lfo1:target", 1, 3, true);
    const s = api.st();
    const r = s.lfoTargetFlatRows[s.lfoTargetFlatIndex];
    if (!r || !r.route || r.route.param !== "cut") bad("host opened on the wrong row: " + (r && r.label));
    api.lfoTargetFlatCommit();
    if (log.commits.length) bad("an unmoved release committed: " + JSON.stringify(log.commits));
  }
  /* FILE: null read refuses; an unlisted current file + unmoved release loads nothing. */
  {
    const items = [{ kind: "file", path: "/r/kits/a.json", label: "a" }, { kind: "dir", path: "/r/kits/x", label: "x" },
                   { kind: "file", path: "/r/kits/b.json", label: "b" }];
    const info = { key: "kit", meta: { type: "filepath", name: "Kit" } };
    let w = world({ file: null, items });
    w.api.fileFlatEnter(0, "synth:kit", 2, true, info);
    if (w.log.views.length) bad("a null file read opened the list");
    w = world({ file: "/elsewhere/k.json", items });
    w.api.fileFlatEnter(0, "synth:kit", 2, true, info);
    if (w.log.views[0] !== "fileflat") bad("a readable cell did not open");
    w.api.fileFlatCommit();
    if (w.log.writes.length) bad("an unmoved release LOADED a file: " + w.log.writes);
    w = world({ file: "/r/kits/a.json", items });
    w.api.fileFlatEnter(0, "synth:kit", 2, true, info);
    w.api.fileFlatMove(1);
    w.api.fileFlatCommit();
    if (w.log.writes.join() !== "kit=/r/kits/b.json") bad("a moved cursor must load its file: " + w.log.writes);
    w = world({ file: "", items: [] });
    w.api.fileFlatEnter(0, "synth:kit", 2, true, info);
    const reads = w.log.reads;
    w.api.fileFlatEnter(0, "synth:kit", 2, true, info);
    if (w.log.reads !== reads || w.log.said.length !== 1) bad("an empty folder re-read or re-announced per detent");
  }
}

if (fail) process.exit(1);
'

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
ui=src/shadow/shadow_ui.js
grep -q 'planFlatTargetOpen(' "$ui" || bad "LFO Target list is not opened through planFlatTargetOpen"
grep -q 'flatTargetCommitRow(lfoTargetFlatRows, lfoTargetFlatIndex, lfoTargetFlatStored)' "$ui" \
  || bad "LFO Target commit does not skip an unmoved cursor"
grep -q 'if (storedTarget === null || storedParam === null) {' "$ui" || bad "a null target read still opens the list"
grep -q 'fileFlatPick(fileFlatRows, fileFlatIndex, fileFlatStored, fileFlatMoved)' "$ui" \
  || bad "file commit does not require a moved cursor"
grep -q 'planFileFlatOpen(current, files)' "$ui" || bad "a null file read still opens the list"
if grep -q 'getSlotParam(slotIndex, fullKey) || ""' "$ui"; then bad "a failed file read is collapsed to empty"; fi
grep -q 'fileFlatAfterCommit(f.path)' "$ui" || bad "the file list skips the browser commit hooks"
grep -q 'if (lfoTargetFromGrid && !isLfoTargetView(view)) lfoTargetFromGrid = false;' "$ui" \
  || bad "the grid hand-off flag survives a non-commit exit"
grep -q 'flatRefusal.latched(fullKey, now)' "$ui" || bad "a refused open repeats on every detent"

[ $fail -eq 0 ] && echo "PASS: a turn that opens a list never commits by itself; failed reads refuse"
exit $fail
