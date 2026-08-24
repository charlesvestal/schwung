#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The keyboard is drawn ON TOP of the knob grid, so it must be FED first.
#
# `onMidiMessageInternal` gives the knob grid an early-out before any of the
# per-view switches, and the text-entry handler sits ~100 lines below it. That
# ordering was safe only for as long as no keyboard could be raised while
# PARAM_PAGES was the live view. User Presets became a trailing page INSIDE the
# grid, and `enterPresetSaveAs` opens the keyboard without calling setView --
# so `view` stays PARAM_PAGES and the grid ate the jog while the keyboard was
# on screen: pad typing worked, the jog paged the menu underneath.
#
# The asymmetry is the whole tell, and it is why this looked like a keyboard
# bug rather than a dispatch-order bug: `decodeInput` returns null for pad
# notes (they fall through to text entry) but decodes CC 14 as navigation (it
# does not). Both halves are pinned below -- a future decodeInput that starts
# claiming pads would break pad typing the same way, silently.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

/* ---- 1. dispatch order: nothing routes to the grid while typing --------- */

const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const start = ui.indexOf("globalThis.onMidiMessageInternal = function(data)");
if (start < 0) {
  fail("could not find onMidiMessageInternal in shadow_ui.js");
} else {
  /* Bound the search at the next top-level globalThis assignment so a match
     from a later handler cannot stand in for one in this one. */
  let end = ui.indexOf("\nglobalThis.", start + 1);
  if (end < 0) end = ui.length;
  const body = ui.slice(start, end);

  const gridCall = body.indexOf("handleParamPagesMidi(");
  if (gridCall < 0) {
    fail("onMidiMessageInternal no longer routes to handleParamPagesMidi");
  } else {
    /* The guard must be reachable BEFORE the grid gets the event. Either
       spelling counts: a condition on the grid block itself, or a text-entry
       early-out hoisted above it. */
    const guard = body.slice(0, gridCall).indexOf("isTextEntryActive()");
    if (guard >= 0) {
      ok("text entry is consulted before the knob grid is offered the event");
    } else {
      fail("the knob grid gets MIDI before isTextEntryActive() is checked -- " +
           "the jog will page the grid underneath an open keyboard");
    }
  }
}

/* ---- 2. the asymmetry that made this look like a keyboard bug ----------- */

const { pathToFileURL } = require("url");
const url = pathToFileURL(process.cwd() + "/src/shared/param_pages/page_input.mjs");

import(url).then(({ decodeInput }) => {
  const mods = { shift: false, mute: false };

  const jog = decodeInput([0xb0, 14, 1], mods);
  if (jog) ok("the grid DOES claim jog turns (CC 14) -- so it must be gated");
  else fail("decodeInput no longer claims CC 14; this test pins the wrong key");

  for (const note of [68, 84, 99]) {
    const pad = decodeInput([0x90, note, 100], mods);
    if (pad === null) ok("the grid does not claim pad note " + note);
    else fail("decodeInput now claims pad note " + note + " (" + JSON.stringify(pad) +
              ") -- pad typing over the grid would break the same way the jog did");
  }
}).then(() => process.exit(failures ? 1 : 0),
        (e) => { fail("could not load page_input.mjs: " + e); process.exit(1); });
'
