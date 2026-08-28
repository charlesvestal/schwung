#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Editor routing between the knob grid and the screens it hands off to.
#
# Two questions, and until now neither was observable from outside
# shadow_ui.js, which is why three routing bugs shipped in a row:
#
#   1. Does clicking a divable cell open the RIGHT editor? A wav_position must
#      show the waveform, a filepath must show the file browser. Clicking
#      Position used to open the module's hierarchy menu with a generic numeric
#      overlay on top, because openParamEditorFromGrid entered the hierarchy
#      editor and stopped without selecting anything.
#
#   2. Does Back come back to the GRID? Every exit from those editors used to
#      land in the hierarchy LIST — a screen the user never opened and, with
#      Param View = Knobs, cannot leave except by exiting the component.
#      Committing a sample file did the same thing.
#
# This is a FUNCTIONAL test: it loads the real shadow_ui.js in node against a
# fake device, drives it with real Move MIDI, and asserts on real view state.
# The rest of tests/shadow/ is source-invariant grep, which cannot see any of
# this. The QuickJS-only 'os'/'std' imports and the deployed
# /data/UserData/schwung/ paths are rewritten into a scratch copy of src/.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the editor-routing tests" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "FAIL: python3 is required to stage the scratch tree" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
REPO="$PWD"

cp -R src "$TMP/src"
TMP="$TMP" python3 - <<'PY'
import os
TMP = os.environ["TMP"]
root = TMP + "/src"
open(root + "/os.mjs", "w").write("export function open(){return null}\nexport const O_RDONLY=0;\nexport default {};\n")
open(root + "/std.mjs", "w").write("export function open(){return null}\nexport function loadFile(){return null}\nexport default {};\n")
for base, _, files in os.walk(root):
    for f in files:
        if not f.endswith((".mjs", ".js")):
            continue
        p = os.path.join(base, f)
        try:
            s = open(p, encoding="utf8").read()
        except Exception:
            continue
        o = s
        s = s.replace("/data/UserData/schwung/", root + "/")
        for m in ("os", "std"):
            s = s.replace("from '%s'" % m, "from '%s/%s.mjs'" % (root, m))
            s = s.replace('from "%s"' % m, 'from "%s/%s.mjs"' % (root, m))
        if s != o:
            open(p, "w", encoding="utf8").write(s)
# shadow_ui.js is a module but named .js; give node an .mjs to import.
s = open(root + "/shadow/shadow_ui.js", encoding="utf8").read()
open(root + "/shadow/ui.mjs", "w", encoding="utf8").write(s)
PY

REPO="$REPO" TREE="$TMP/src" node --input-type=module -e '
const TREE = process.env.TREE;
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* ---- device + host globals the UI draws and talks through --------------- */
for (const n of ["print","fill_rect","clear_screen","text_width","draw_line",
                 "draw_circle","fill_circle","draw_arc","flush_display",
                 /* the knob indicator ring LEDs go out through this one */
                 "move_midi_internal_send",
                 /* the wav_position editor draws through these two, and it is
                    the only screen here that does -- without them the spray
                    case below throws instead of asserting. */
                 "draw_rect","set_pixel"]) {
  globalThis[n] = () => 0;
}

/*
 * A granny-shaped contract: the case that exposed the bug. `position` is a
 * ranged wav_position (turnable AND divable) and `sample_path` is a filepath
 * (divable only), on one level, so both routes are reachable from one page.
 */
const FILLER = ["a","b","c","d","e","f","g","h"];
const CHAIN_PARAMS = [
  { key: "gain",        name: "Gain",        type: "float", min: 0, max: 1, step: 0.01 },
  ...FILLER.map((k) => ({ key: k, name: k.toUpperCase(), type: "float", min: 0, max: 1, step: 0.01 })),
  { key: "position",    name: "Position",    type: "wav_position", mode: "position",
    filepath_param: "sample_path", min: 0, max: 1, step: 0.01 },
  { key: "sample_path", name: "Sample File", type: "filepath",
    root: "/tmp", filter: ".wav" },
  /* The granular read spread. Present so the wav_position editor has fences to
     draw -- see the spray case at the bottom of this file. */
  { key: "spray", name: "Spray", type: "float", min: 0, max: 1, step: 0.001 },
  /* A wav_position reachable from a ROOT knob while living in `main`. granny
     is shaped this way -- root lists only navigation entries, so diving from a
     root page RELOCATES the editor to another level on the way in. Without
     this the fixture never exercises that hop. */
  { key: "position2", name: "Position 2", type: "wav_position", mode: "position",
    filepath_param: "sample_path", min: 0, max: 1, step: 0.01 },
  /* An enum, reported by NAME — the chord case. Divable now (clicking a held
   * enum knob opens its option list) while the knob still steps it. */
  { key: "mode", name: "Mode", type: "enum",
    options: ["Hall", "Room", "Plate", "Cave"] },
];
/*
 * Shaped like granny ON PURPOSE: root puts the params on KNOBS but its params[]
 * holds navigation entries only, so `position` is reachable from a root knob
 * and is listed in no root param array. It lives in `main`. A fixture that put
 * position in root.params[] passed while the device failed — the grid searched
 * the page level, found nothing, and fell through to the module menu.
 */
const HIERARCHY = {
  modes: null,
  levels: {
    root: {
      label: "Granny",
      /* NOT position/sample_path: putting them on a root knob lands them on
       * page 0, and the page-restore assertion below then passes whether or
       * not the restore happens. They must be reachable only via an OVERFLOW
       * page of `main`, which is where the real complaint came from. */
      knobs: ["gain", "position2"],
      params: [{ level: "main", label: "Main" }],
    },
    /* Nine params before the divable pair, so they land on an OVERFLOW page:
     * returning to page 1 after editing something on page 2 is the bug below. */
    main: {
      label: "Main",
      knobs: ["gain", ...FILLER],
      params: [{ key: "gain" }, ...FILLER.map((k) => ({ key: k })),
               { key: "position" }, { key: "spray" }, { key: "sample_path" },
               { key: "mode" }, { key: "position2" }],
    },
  },
};
const values = { gain: "0.5", position: "0.5", position2: "0.25", spray: "0.125",
                 sample_path: "", mode: "Hall" };
for (const k of FILLER) values[k] = "0.5";
function getParam(key) {
  const bare = String(key).replace(/^[^:]+:/, "");
  if (bare === "ui_hierarchy")  return JSON.stringify(HIERARCHY);
  if (bare === "chain_params")  return JSON.stringify(CHAIN_PARAMS);
  if (bare === "module")        return "granny";
  if (bare in values)           return values[bare];
  return "";
}
function setParam(key, v) {
  const bare = String(key).replace(/^[^:]+:/, "");
  values[bare] = String(v);
}

/*
 * Stub at the REAL boundary. shadow_ui.js has its own getSlotParam/setSlotParam
 * built on the shadow_get_param/shadow_set_param host globals; overriding only
 * ctx.getSlotParam reaches the grid module and nothing else, and the hierarchy
 * lookup then comes back empty and falls through to the component-edit
 * fallback instead of the editor under test.
 */
globalThis.shadow_get_param = (slot, key) => getParam(key);
globalThis.shadow_set_param = (slot, key, v) => { setParam(key, v); return true; };
globalThis.shadow_get_shift_held = () => 0;
globalThis.host_file_exists = () => true;
globalThis.host_read_file = () => "";
globalThis.host_write_file = () => true;

const ui  = await import(TREE + "/shadow/ui.mjs");
const { ctx } = await import(TREE + "/shadow/shadow_ui_ctx.mjs");
const V   = await import(TREE + "/shadow/shadow_ui_param_pages.mjs");

/*
 * Param View = Knobs. shadow_ui.js sets this from persisted config in init(),
 * which the harness never calls, and paramPagesEnabled() reads the global at
 * call time — so overriding it here is exactly what the setting would do.
 * Without it every entry point falls back to the LIST and the grid routing
 * under test never runs at all.
 */
globalThis.param_view_get_mode = () => 1;
globalThis.tts_get_enabled = () => false;

ctx.getSlotParam = (slot, key) => getParam(key);
ctx.setSlotParam = (slot, key, v) => setParam(key, v);

if (typeof ctx.activeParamEditor !== "function") {
  fail("ctx.activeParamEditor is missing — editor routing is unobservable again");
}

/* ---- drive the grid ------------------------------------------------------ */
const noteOn  = (slot) => [0x90, slot, 100];
const noteOff = (slot) => [0x80, slot, 0];
const click   = () => [0xb0, 3, 127];
const back    = () => [0xb0, 51, 127];

/*
 * Route input the way the device does: the grid module only sees MIDI while IT
 * is the view. Once a hand-off has happened the events belong to shadow_ui.js
 * onMidiMessageInternal. Driving Back through handleParamPagesMidi tested
 * nothing — the editor never received it.
 */
function feed(msg) {
  if (ctx.view === ctx.VIEWS.PARAM_PAGES) V.handleParamPagesMidi(msg);
  else globalThis.onMidiMessageInternal(Uint8Array.from(msg));
}

/*
 * The first button press after load is eaten by the splash skip, exactly as on
 * a real boot. Consume it once with a harmless modifier so the assertions below
 * measure routing rather than the splash.
 */
globalThis.onMidiMessageInternal(Uint8Array.from([0xb0, 49, 127]));
globalThis.onMidiMessageInternal(Uint8Array.from([0xb0, 49, 0]));

function openGrid() {
  V.enterParamPages(0, "synth", "synth");
  /* Land on page 0 explicitly. A previous case may have left the controller on
   * the last page, and the jog does not wrap — gotoSlotFor would then walk
   * forward forever without finding a key that is behind it. */
  V.paramPagesGoTo(0);
  for (let i = 0; i < 24; i++) V.tickParamPages();
}

function slotFor(name) {
  const page = V.currentParamPage();
  if (!page || !page.keys) return -1;
  for (let i = 0; i < page.keys.length; i++) if (page.keys[i] === name) return i;
  return -1;
}

/* Jog forward until `name` is on the visible page. Returns its slot, or -1. */
function gotoSlotFor(name) {
  for (let hop = 0; hop < 40; hop++) {
    const s = slotFor(name);
    if (s >= 0) return s;
    feed([0xb0, 14, 1]);          /* jog one page forward */
    for (let i = 0; i < 6; i++) V.tickParamPages();
  }
  return -1;
}

/* ---- 1. a wav_position opens the WAVEFORM, not the module menu ---------- */
{
  openGrid();
  const slot = gotoSlotFor("position");
  if (slot < 0) {
    fail("position never reached the grid");
  } else {
    const pageName = (V.currentParamPage() || {}).name;
    feed(noteOn(slot));
    feed(click());
    const editor = ctx.activeParamEditor();
    if (editor !== "wav_position") {
      fail("clicking a wav_position opened " + JSON.stringify(editor) +
           ", expected \"wav_position\" — a generic \"value\" here is the module " +
           "menu with a numeric overlay, which is the bug this pins");
    }
    /* ---- 2. Back from the waveform returns to the GRID, ON THE SAME PAGE -- */
    const fromPage = pageName;
    feed(back());
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("Back from the waveform editor landed on view " + ctx.view +
           ", expected PARAM_PAGES (" + ctx.VIEWS.PARAM_PAGES + ")");
    } else {
      const back2 = V.currentParamPage();
      if (!back2 || back2.name !== fromPage) {
        fail("Back from the waveform landed on page " +
             JSON.stringify(back2 && back2.name) + ", expected " +
             JSON.stringify(fromPage) + " — the page the user was on");
      }
    }
  }
}

/* ---- 3. a filepath opens the FILE BROWSER ------------------------------- */
{
  openGrid();
  const slot = gotoSlotFor("sample_path");
  if (slot < 0) {
    fail("sample_path never reached the grid");
  } else {
    const pageName2 = (V.currentParamPage() || {}).name;
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "filepath") {
      fail("clicking a filepath opened " + JSON.stringify(ctx.activeParamEditor()) +
           ", expected the file browser");
    }
    /* ---- 4. Back from the browser returns to the GRID, ON THE SAME PAGE --- */
    feed(back());
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("Back from the file browser landed on view " + ctx.view +
           ", expected PARAM_PAGES (" + ctx.VIEWS.PARAM_PAGES + ")");
    } else {
      const back4 = V.currentParamPage();
      if (!back4 || back4.name !== pageName2) {
        fail("Back from the file browser landed on page " +
             JSON.stringify(back4 && back4.name) + ", expected " +
             JSON.stringify(pageName2));
      }
    }
  }
}

/* ---- 5. a plain number does NOT hand off at all ------------------------- */
{
  openGrid();
  const slot = gotoSlotFor("gain");
  if (slot < 0) {
    fail("gain never reached the grid");
  } else {
    feed(noteOn(slot));
    feed(click());
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("clicking a plain number left the grid for view " + ctx.view +
           " — only a divable param hands off");
    }
    feed(noteOff(slot));
  }
}

/* ---- 6. footer: jog is slot 1, click is slot 2, and OPEN when divable ---- */
{
  openGrid();
  const flat = (h) => (h || []).map((p) => p.join(" ")).join(" / ");

  const plain = V.paramPagesFooterHints() || [];
  if (!plain.length || plain[0][0] !== "JOG") {
    fail("plain footer must lead with JOG, got " + JSON.stringify(flat(plain)));
  }
  if (plain.length < 2 || plain[1][0] !== "CLK") {
    fail("plain footer must put CLK second, got " + JSON.stringify(flat(plain)));
  }

  const slot = gotoSlotFor("position");
  if (slot < 0) {
    fail("position never reached the grid for the footer check");
  } else {
    feed(noteOn(slot));
    const held = V.paramPagesFooterHints() || [];
    if (!held.length || held[0][0] !== "JOG" || held[1][0] !== "CLK") {
      fail("held-knob footer must still be JOG then CLK — the slots are " +
           "positional so the eye stops re-reading them — got " + JSON.stringify(flat(held)));
    }
    if (held[1][1] !== "OPEN") {
      fail("holding a divable knob must say CLK OPEN, got " + JSON.stringify(flat(held)) +
           " — position is divable but classifies as a NUMBER, so a check on " +
           "kind === opaque misses it and the footer says MENU while the button " +
           "opens the editor");
    }
    feed(noteOff(slot));
  }

  /* Every state must fit the 128px band: drawFooter drops the tail rather than
   * squeezing, and a silently dropped hint is how CLK GO went missing. */
  const RM = await import(TREE + "/shared/param_pages/render_page_movy.mjs");
  const width = (h) => (h || []).reduce((a, [k, v]) => a + RM.hintPairWidth(k, v), 1);
  for (const h of [plain, V.paramPagesFooterHints()]) {
    if (width(h) > 128) fail("footer overflows: " + flat(h) + " = " + width(h) + "px");
  }
}

/* ---- 7. an ENUM opens the OPTION PICKER, and the knob still turns it ----
 *
 * Every enum is a door now. The knob keeps stepping the value -- that is what
 * makes this different from the opaque types, which a knob cannot drive at all
 * -- so both abilities are asserted on the same param.
 *
 * There is deliberately NO bracket mark on an enum cell (see
 * test_enum_picker.sh); the whole affordance is the footer flipping to
 * CLK OPEN, which is why that is checked here rather than left implicit.
 */
{
  openGrid();
  const slot = gotoSlotFor("mode");
  if (slot < 0) {
    fail("mode never reached the grid");
  } else {
    const pageName = (V.currentParamPage() || {}).name;
    feed(noteOn(slot));

    const held = V.paramPagesFooterHints() || [];
    if (!held.length || held[1] === undefined || held[1][1] !== "OPEN") {
      fail("holding an ENUM knob must say CLK OPEN, got " +
           JSON.stringify((held || []).map((p) => p.join(" ")).join(" / ")) +
           " -- the footer is the ONLY affordance an enum gets, because the " +
           "bracket mark is deliberately withheld from enums");
    }

    feed(click());
    if (ctx.activeParamEditor() !== "enum") {
      fail("clicking a held enum opened " + JSON.stringify(ctx.activeParamEditor()) +
           ", expected the option picker. A \"value\" here is the inline nudge " +
           "editor openHierarchyParamEditor falls through to, which is the gap " +
           "this fills");
    }
    /*
     * The grid CONTROLLER must survive. An enum picker does not go the long way
     * round through the list editor the way a filepath does — it has the
     * options and the index already — so the grid is not torn down and rebuilt,
     * and the commit closure it handed over is still pointing at a live
     * controller. If this is null the hand-off has gone through
     * openParamEditorFromGrid, which calls exitParamPages(), and the commit
     * below would write through a corpse.
     */
    if (!V.currentParamPage()) {
      fail("the grid was torn down to open the option picker -- the enum path " +
           "must keep the controller alive, the way the LFO target picker does");
    }

    /* Scroll one and commit. The value must go out as a NAME: this fixtures
     * plugin reports "Hall", so an index would be silently discarded by a
     * strcmp ladder (the chord bug). */
    feed([0xb0, 14, 1]);
    feed(click());
    if (values.mode !== "Room") {
      fail("committing the picker wrote " + JSON.stringify(values.mode) +
           ", expected \"Room\" -- the wire format must be auto-detected from " +
           "what the plugin reports, not hardcoded to String(index)");
    }
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("committing the picker landed on view " + ctx.view + ", expected the grid");
    } else {
      const b = V.currentParamPage();
      if (!b || b.name !== pageName) {
        fail("committing the picker landed on page " + JSON.stringify(b && b.name) +
             ", expected " + JSON.stringify(pageName));
      }
      /*
       * The note-off for the held knob went to the PICKER, so the grid never
       * saw it. Unless the hand-off drops the touch, that cell stays
       * highlighted for good -- which is exactly what clearParamPagesTouch
       * exists for. Observed through the footer, which says CLK OPEN only
       * while a divable knob is held.
       */
      const after = V.paramPagesFooterHints() || [];
      if (after[1] && after[1][1] === "OPEN") {
        fail("the grid still thinks the enum knob is held after the picker " +
             "returned -- the note-off went to the picker, so the hand-off " +
             "must clear the touch");
      }
    }
    feed(noteOff(slot));
  }
}

/* ---- 8. Back out of the picker leaves the value ALONE ------------------- */
{
  openGrid();
  const slot = gotoSlotFor("mode");
  if (slot < 0) {
    fail("mode never reached the grid for the cancel case");
  } else {
    const before = values.mode;
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "enum") fail("the picker did not open for the cancel case");
    feed([0xb0, 14, 1]);
    feed([0xb0, 14, 1]);
    if (values.mode !== before) {
      fail("scrolling the picker wrote " + JSON.stringify(values.mode) +
           " -- scrolling must not commit, or Back has nothing to cancel");
    }
    feed(back());
    if (values.mode !== before) {
      fail("Back out of the picker changed the value to " + JSON.stringify(values.mode) +
           ", expected it untouched at " + JSON.stringify(before));
    }
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("Back out of the picker landed on view " + ctx.view + ", expected the grid");
    }
    feed(noteOff(slot));
  }
}

/* ---- 8b. the knob still STEPS the enum -------------------------------- */
{
  openGrid();
  const slot = gotoSlotFor("mode");
  if (slot < 0) {
    fail("mode never reached the grid for the turn case");
  } else {
    const before = values.mode;
    feed(noteOn(slot));
    for (let i = 0; i < 24; i++) feed([0xb0, 71 + slot, 1]);
    for (let i = 0; i < 8; i++) V.tickParamPages();
    feed(noteOff(slot));
    for (let i = 0; i < 4; i++) V.tickParamPages();
    if (values.mode === before) {
      fail("turning the enum knob no longer moves it (still " + JSON.stringify(before) +
           ") -- making it divable must not make it opaque");
    }
    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("turning the enum knob left the grid, view=" + ctx.view);
    }
  }
}

/* ---- 9. SLOT SETTINGS opens as a grid and its menu runs the real actions -- */
{
  const slotStore = {
    "slot:volume": "1.00", "slot:muted": "0", "slot:soloed": "0",
    "slot:transpose": "0", "slot:receive_channel": "1",
    "slot:forward_channel": "-1", "midi_fx_pre_mode": "0",
  };
  const prevGet = globalThis.shadow_get_param, prevSet = globalThis.shadow_set_param;
  globalThis.shadow_get_param = (slot, key) => (key in slotStore ? slotStore[key] : getParam(key));
  globalThis.shadow_set_param = (slot, key, v) => { slotStore[key] = String(v); return true; };

  V.exitParamPages();
  ctx.enterChainSettings(0);
  for (let i = 0; i < 12; i++) V.tickParamPages();

  if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
    fail("slot settings did not open as the knob grid, view=" + ctx.view);
  } else {
    const p0 = V.currentParamPage();
    if (!p0 || p0.kind !== "knobs") fail("slot settings page 1 should be a knob grid, got " + (p0 && p0.kind));
    for (const want of ["volume", "muted", "soloed", "transpose",
                        "receive_channel", "forward_channel", "midi_fx_pre_mode", "mpe_mode"]) {
      if (!((p0 && p0.keys) || []).includes(want)) {
        fail("slot grid page 1 is missing " + want + ": " + JSON.stringify(p0 && p0.keys));
      }
    }

    /* The io must be reaching the real store, not a component. */
    if (V.paramPagesComponent() !== "slot") fail("the grid is not pointed at the slot");

    /* ---- 9a. the enum picker on a SYNTHESISED contract --------------------
     *
     * Slot settings is where the picker earns its keep — Fwd Ch has eighteen
     * options — and it is also the case that cannot go through the list
     * editor at all: "slot" has no ui_hierarchy, so openParamEditorFromGrid
     * refuses everything but the LFO target. The enum path therefore must NOT
     * be routed through there.
     *
     * And the commit must go back through the CONTROLLER, not straight to
     * setSlotParam: Fwd is stored offset (Thru = -2, Auto = -1) by the slot
     * io, so a picker that wrote the raw index would move every channel two
     * places. Index 3 is "Ch 2", which the io stores as 1.
     */
    {
      const fwd = ((V.currentParamPage() || {}).keys || []).indexOf("forward_channel");
      if (fwd < 0) {
        fail("forward_channel is not on the slot grid page");
      } else {
        feed(noteOn(fwd));
        feed(click());
        if (ctx.activeParamEditor() !== "enum") {
          fail("clicking Fwd Ch on slot settings opened " +
               JSON.stringify(ctx.activeParamEditor()) + " — a synthesised " +
               "contract has no hierarchy to fall back to, so this must not be " +
               "routed through the list editor");
        } else {
          feed([0xb0, 14, 1]);   /* Auto -> Ch 1 */
          feed([0xb0, 14, 1]);   /* Ch 1  -> Ch 2 */
          feed(click());
          if (slotStore["slot:forward_channel"] !== "1") {
            fail("the picker stored forward_channel as " +
                 JSON.stringify(slotStore["slot:forward_channel"]) +
                 ", expected \"1\" (Ch 2) — the commit must go through the slot " +
                 "io, which offsets Thru/Auto, not straight to setSlotParam");
          }
          if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
            fail("committing on slot settings landed on view " + ctx.view);
          }
        }
        feed(noteOff(fwd));
        for (let i = 0; i < 4; i++) V.tickParamPages();
      }
    }

    /* A slot has no module to abbreviate, so the header read "S1 > ---". */
    const titles = [];
    const realPrint = globalThis.print;
    globalThis.print = (x, y, t) => { if (y < 8) titles.push(String(t)); return 0; };
    V.drawParamPages();
    globalThis.print = realPrint;
    if (titles.some((t) => /---/.test(t))) {
      fail("the slot grid header shows a missing module abbreviation: " + JSON.stringify(titles));
    }

    /* Walk to the actions menu and activate an entry through the REAL input
     * path — the menu is inert until entered, so this is two clicks. */
    let guard = 0, page = V.currentParamPage();
    while (page && page.kind !== "menu" && guard++ < 20) {
      feed([0xb0, 14, 1]);
      for (let i = 0; i < 4; i++) V.tickParamPages();
      page = V.currentParamPage();
    }
    if (!page || page.kind !== "menu") {
      fail("slot settings has no actions menu page");
    } else {
      const labels = (page.entries || []).map((e) => e.label);
      /* LFO 1 and LFO 2 are PAGES now — eight of their nine params are
       * turnable and the widgets draw the thing itself. */
      if (labels.includes("LFO 1")) fail("LFO 1 should be a page, not a menu entry");
      for (const want of ["Knob Mapping", "Save"]) {
        if (!labels.includes(want)) fail("actions menu missing " + want + ": " + JSON.stringify(labels));
      }
      /*
       * The host draws the grid by calling drawParamPages() and, if it returns
       * false, running enterHierarchyEditorFromParamPages() instead. A menu
       * page that refuses to draw therefore EJECTS to the hierarchy editor for
       * component "slot", which has no ui_hierarchy — so jogging to the actions
       * page landed on "No presets". The draw must claim this page.
       */
      if (V.drawParamPages() !== true) {
        fail("drawParamPages refused the MENU page — the host fallback then " +
             "enters the hierarchy editor for the slot, which lands on No presets");
      }
      if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
        fail("drawing the menu page left the grid, view=" + ctx.view);
      }

      /* Enter, land on Knob Mapping, activate. The whole point of the wiring is
       * that this runs the SAME action the list runs, so the proof is the view
       * it lands on — not that nothing threw. */
      feed(click());
      feed(click());
      if (ctx.view !== ctx.VIEWS.KNOB_EDITOR) {
        fail("activating Knob Mapping from the grid menu did not open the knob editor, view=" +
             ctx.view + " — the menu intent is not reaching runChainSettingAction");
      }
    }
  }
  /* ---- 9b. entering slot settings DIRECTLY from a module grid ----------- */
  /*
   * The sequence that actually broke on hardware. Two pieces of state survive
   * an entry and both were wrong:
   *
   *   - the controller CLOSES OVER its accessors, so one built for a module
   *     keeps reading the module unless the io change forces a rebuild;
   *   - suppressParamPagesOnce is set by a component hand-off, and sharing it
   *     with slot settings made the next slot entry silently show the LIST.
   *
   * So: open a module grid, hand a param off to an editor (which sets the
   * flag), then go straight to slot settings WITHOUT exiting first.
   */
  {
    V.exitParamPages();
    V.enterParamPages(0, "synth", "synth");
    for (let i = 0; i < 12; i++) V.tickParamPages();
    const sp = gotoSlotFor("sample_path");
    if (sp >= 0) {
      feed(noteOn(sp));
      feed(click());            /* opens the filepath browser, sets the flag */
      feed(back());             /* back to the grid */
      feed(noteOff(sp));
    }

    ctx.enterChainSettings(0);
    for (let i = 0; i < 12; i++) V.tickParamPages();

    if (ctx.view !== ctx.VIEWS.PARAM_PAGES) {
      fail("after a module param hand-off, slot settings fell back to the LIST (view=" +
           ctx.view + ") — the component one-shot flag is leaking into the slot path");
    }
    const pg = V.currentParamPage();
    const keys = (pg && pg.keys) || [];
    if (!keys.includes("volume")) {
      fail("slot settings showed the MODULE pages (" + JSON.stringify(keys) +
           ") — the controller closes over its accessors and was not rebuilt when the io changed");
    }
  }

  globalThis.shadow_get_param = prevGet;
  globalThis.shadow_set_param = prevSet;
}

/* ---- N. the wav_position editor draws the SPRAY FENCES ------------------
 *
 * granny draws the spray as two dotted fences around the cursor in its grid
 * cell, and the fullscreen editor drew nothing -- the word "spray" appeared
 * nowhere in shadow_ui.js. That mattered once a click on the spray cell
 * started opening this editor: the user dove from the control and landed on a
 * screen that did not show it.
 *
 * Asserted on the PIXELS the editor puts out, not on the source, because the
 * risky part is arithmetic -- wrap, clamp, zoom -- and all of it reads
 * plausibly either way.
 *
 * DOTTED vs SOLID is the discriminator: the cursor is a solid column and a
 * fence is lit on alternating rows, which is the whole visual distinction
 * between "where a grain is read" and "how far it may wander".
 *
 * THE CURSOR SITS AT RATIO 0 HERE, and that is not a defect in the fixture.
 * There is no real WAV behind the stub, so the duration is 0 and the position
 * cannot be mapped to a ratio -- which puts this case squarely on the WRAPPING
 * path, where pos - spray goes negative and has to come back round to
 * 1 - spray. That is the arithmetic most likely to be wrong, so it is the
 * better thing to pin than a comfortable mid-file cursor would be.
 *
 * Driven at TWO spray values, because a single one is satisfied by a pair of
 * hard-coded columns.
 */
{
  /* A FILE, for this case only. The fixture leaves sample_path empty (the
   * browser case needs that). host_file_exists is stubbed true, so the path
   * only has to be non-empty. */
  const savedPath = values.sample_path, savedSpray = values.spray;

  const PLOT_X0 = 4;      /* plotX + 1, mirroring drawWavPositionEditor */
  const INNER_W = 120;    /* plotW - 2 */

  function captureFences(sprayValue) {
    values.sample_path = "/tmp/probe.wav";
    values.spray = String(sprayValue);
    openGrid();
    const slot = gotoSlotFor("position");
    if (slot < 0) return null;
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "wav_position") return null;

    const cols = new Map();
    const realPixel = globalThis.set_pixel;
    globalThis.set_pixel = (x, y, c) => {
      if (c) { if (!cols.has(x)) cols.set(x, []); cols.get(x).push(y); }
      return 0;
    };
    ctx.drawScreen ? ctx.drawScreen() : globalThis.tick();
    globalThis.set_pixel = realPixel;
    feed(back());

    const tall = [...cols.entries()].filter(([, ys]) => ys.length >= 8);
    const run = (ys, step) => {
      const t = [...ys].sort((a, b) => a - b);
      return t.length > 1 && t.every((y, i) => i === 0 || y === t[i - 1] + step);
    };
    return {
      solid:  tall.filter(([, ys]) => run(ys, 1)).map(([x]) => x).sort((a, b) => a - b),
      dotted: tall.filter(([, ys]) => run(ys, 2)).map(([x]) => x).sort((a, b) => a - b),
    };
  }

  const expectAt = (frac) => PLOT_X0 + Math.round(frac * (INNER_W - 1));

  for (const spray of [0.125, 0.25]) {
    const r = captureFences(spray);
    if (!r) { fail("the spray case could not reach the waveform editor at spray=" + spray); continue; }
    if (r.solid.length < 1) {
      /* Two causes, and both matter. Either the capture stopped seeing the
       * editor at all, or a fence landed ON the cursor column and made it read
       * as mixed rather than solid -- which is what CLAMPING instead of
       * wrapping does at ratio 0, so this is a real assertion and not just a
       * sanity check on the harness. */
      fail("no solid cursor column at spray=" + spray +
           " — either the capture broke, or a fence landed on the cursor " +
           "column (a clamped fence does that at ratio 0; it must wrap)");
      continue;
    }
    if (r.dotted.length !== 2) {
      fail("the wav_position editor drew " + r.dotted.length + " dotted fence column(s) " +
           "at spray=" + spray + ", expected 2 — a click on spray opens this screen, " +
           "so the spread has to be visible on it (cols " + JSON.stringify(r.dotted) + ")");
      continue;
    }
    /* Cursor at 0, so the fences land at +spray and at the WRAPPED 1 - spray. */
    const want = [expectAt(spray), expectAt(1 - spray)].sort((a, b) => a - b);
    for (let i = 0; i < 2; i++) {
      if (Math.abs(r.dotted[i] - want[i]) > 1) {
        fail("fence " + i + " at x=" + r.dotted[i] + " for spray=" + spray +
             ", expected ~" + want[i] + " — the fences are not tracking the value " +
             "(got " + JSON.stringify(r.dotted) + ", want " + JSON.stringify(want) + ")");
      }
    }
  }

  /* Spray OFF draws no fences at all: two columns sitting on top of the cursor
   * would read as a spread of nothing. */
  const off = captureFences(0);
  if (off && off.dotted.length !== 0) {
    fail("spray=0 still drew " + off.dotted.length + " fence(s) — at zero both " +
         "land on the cursor and state a spread that is not there");
  }

  values.sample_path = savedPath;
  values.spray = savedSpray;
}

/* ---- the editor says where a SOURCE has the position --------------------
 *
 * The editor is the screen you open specifically to SET this value. In edit
 * mode the cursor is hierEditorEditValue, seeded from `:base` -- correctly,
 * that is what the jog moves -- so with an LFO running the cursor sits still
 * while the sound sweeps and nothing on screen says why.
 *
 * Asserted as a STUB PAIR, not merely as "extra ink": the mark has to be
 * distinguishable from the two things already in this plot, a solid
 * full-height cursor and dotted full-height spray fences. A column of 4 pixels
 * split between the top and the bottom of the band is none of those, and a
 * mark drawn as a rule would satisfy an ink-only check while reading as a
 * third fence.
 *
 * The position is fed as "0.5" so it maps to mid-file (the value is
 * normalised against the declared min/max, NOT read as a percentage): the base resolves to
 * ratio 0 in this fixture (no real WAV behind the stub, see the spray case
 * above), so a mid-file source is what makes the two columns DIFFER. Fed the
 * same value as the base, the assertion could pass on the cursor.
 */
{
  const savedPath = values.sample_path, savedPos = values.position;
  const savedSpray2 = values.spray;
  const PLOT_X0 = 4, INNER_W = 120;

  function captureCols(modulated) {
    values.sample_path = "/tmp/probe.wav";
    values.spray = "0";                    /* no fences competing */
    values.position = "0.5";               /* mid-file for the SOURCE */
    if (modulated) values["position:modulated"] = "1";
    else delete values["position:modulated"];
    openGrid();
    const slot = gotoSlotFor("position");
    if (slot < 0) return null;
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "wav_position") return null;
    const cols = new Map();
    const realPixel = globalThis.set_pixel;
    globalThis.set_pixel = (x, y, c) => {
      if (c) { if (!cols.has(x)) cols.set(x, []); cols.get(x).push(y); }
      return 0;
    };
    ctx.drawScreen ? ctx.drawScreen() : globalThis.tick();
    globalThis.set_pixel = realPixel;
    feed(back());
    return cols;
  }

  /* A COARSE DASH: runs of exactly 2, separated by gaps. That rhythm is what
   * tells it apart from the two lines already in this plot -- the SOLID cursor
   * (one run spanning the band) and the FINE spray dither (every run length 1).
   * An ink-only check passes for all three. */
  const stubCols = (cols) => [...cols.entries()].filter(([, ys]) => {
    const t = [...new Set(ys)].sort((a, b) => a - b);
    if (t.length < 4) return false;
    const runs = [];
    for (const y of t) {
      const last = runs[runs.length - 1];
      if (last && y === last[last.length - 1] + 1) last.push(y);
      else runs.push([y]);
    }
    if (runs.length < 2) return false;              /* solid */
    if (runs.every((r) => r.length === 1)) return false;  /* fine dither */
    return runs.every((r) => r.length <= 2);
  }).map(([x]) => x);

  const on  = captureCols(true);
  const off = captureCols(false);

  if (!on || !off) {
    fail("the modulation case could not reach the waveform editor");
  } else {
    const marks = stubCols(on);
    if (marks.length !== 1) {
      fail("a modulated wav_position drew " + marks.length + " source mark(s) in the " +
           "editor, expected 1 — the screen you set the value on cannot say where " +
           "the LFO has it (cols " + JSON.stringify(marks) + ")");
    } else {
      const want = PLOT_X0 + Math.round(0.5 * (INNER_W - 1));
      if (Math.abs(marks[0] - want) > 1)
        fail("the source mark is at x=" + marks[0] + ", expected ~" + want +
             " — it is not tracking the modulated position");
    }
    /* And an UNMODULATED position draws none: the mark must state a fact, not
     * decorate every wav_position in the fleet. */
    if (stubCols(off).length !== 0)
      fail("an unmodulated wav_position still drew a source mark");
  }

  values.sample_path = savedPath;
  values.position = savedPos;
  values.spray = savedSpray2;
  delete values["position:modulated"];
}

/* ---- N. the editor inherits the PAGE knob row ---------------------------
 *
 * A declared `knobs` array is not the order the user was just looking at. The
 * grid re-seats keys for LAYOUT -- gatherGroupMembers pulls granny spray next
 * to position so the waveform can span both cells -- so diving into the wave
 * editor silently changed which physical knob was which, one click apart.
 *
 * Found on hardware by turning what looked like the spray knob and watching
 * the log resolve it to `synth:size_ms`. Reported as: "the editor should be
 * using the same knobs as the entered page. using main is confusing, its a
 * hidden order no one has reference to."
 *
 * Asserted through the knob MAPPING rather than by reading hierEditorKnobs:
 * the array is an implementation detail, and which param a physical knob
 * drives is the behaviour.
 *
 * The fixture earns this by declaring knobs and params in DIFFERENT orders --
 * with both the same, the test passes whether or not the override exists.
 */
{
  openGrid();
  const slot = gotoSlotFor("position");
  if (slot < 0) {
    fail("position never reached the grid for the knob-row case");
  } else {
    const pg = V.currentParamPage();
    const pageKeys = ((pg && pg.keys) || []).slice();
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "wav_position") {
      fail("the knob-row case did not reach the waveform editor");
    } else if (typeof ctx.knobParamKey !== "function") {
      fail("ctx.knobParamKey is missing -- the knob mapping is unobservable again");
    } else {
      const mismatches = [];
      for (let i = 0; i < pageKeys.length && i < 8; i++) {
        const want = pageKeys[i];
        if (!want) continue;
        const got = ctx.knobParamKey(i);
        if (got !== want) mismatches.push("knob " + (i + 1) + ": page=" + want + " editor=" + got);
      }
      if (mismatches.length) {
        fail("the editor knob row does not match the page it was entered from: " +
             mismatches.join(" | "));
      }
      feed(back());
    }
  }
}

/* ---- N+1. the row survives the LEVEL HOP that entering performs ---------
 *
 * The page the user was on and the level the param LIVES in are not always the
 * same. granny root lists only navigation entries, so diving from a root page
 * relocates the editor to `main` on the way in -- and the first version of the
 * override read that hop as "the user navigated away" and handed the declared
 * row back. It applied at root and was discarded at main:
 *
 *   knobRow: level=root fromPage=root -> [position, spray, size_ms, ...]
 *   knobRow: level=main fromPage=root -> [position, size_ms, density, spray, ...]
 *
 * The case the ORIGINAL test could not see, because its fixture put the param
 * in the page own level. position2 exists to create the hop.
 */
{
  openGrid();
  V.paramPagesGoTo(0);
  for (let i = 0; i < 24; i++) V.tickParamPages();
  const pg = V.currentParamPage();
  const pageKeys = ((pg && pg.keys) || []).slice();
  const slot = pageKeys.indexOf("position2");
  if (slot < 0) {
    fail("position2 is not on the root page -- the level-hop case is not set up");
  } else {
    feed(noteOn(slot));
    feed(click());
    if (ctx.activeParamEditor() !== "wav_position") {
      fail("diving from the root page did not reach the waveform editor");
    } else {
      const mismatches = [];
      for (let i = 0; i < pageKeys.length && i < 8; i++) {
        const want = pageKeys[i];
        if (!want) continue;
        const got = ctx.knobParamKey(i);
        if (got !== want) mismatches.push("knob " + (i + 1) + ": page=" + want + " editor=" + got);
      }
      if (mismatches.length) {
        fail("after the entry level hop the editor knob row reverted to the " +
             "declared order: " + mismatches.join(" | "));
      }
      feed(back());
    }
  }
}

/* ---- a dive on a CHILD LEVEL opens the editor, not nothing --------------
 *
 * The two halves of a dive speak different dialects. The GRID addresses the
 * concrete key, because the controller resolved the child template
 * (`synth:p01_sample_path`), while the editor selects out of what the LEVEL
 * lists (`sample_path`). So indexOfHierParam missed, findLevelListingParam
 * missed, and the click opened NOTHING -- activeParamEditor stayed null.
 * Reported from the device as the file picker never firing.
 *
 * Driven for real rather than pinned: three source-level fixes to this same
 * dive shipped without moving it, because a grep can confirm a line exists and
 * cannot confirm an editor opened.
 *
 * LAST in the file, because it rewrites the shared fixture into a child level.
 */
{
  HIERARCHY.levels.main.child_count = 4;
  HIERARCHY.levels.main.child_label = "Pad";
  HIERARCHY.levels.main.child_key_template = "p{index}_{key}";
  HIERARCHY.levels.main.child_index_base = 1;
  HIERARCHY.levels.main.child_index_digits = 2;
  HIERARCHY.levels.main.child_index_param = "cur_pad";
  CHAIN_PARAMS.push({ key: "cur_pad", name: "Current", type: "int", min: 1, max: 4 });
  for (let i = 1; i <= 4; i++) {
    const n = String(i).padStart(2, "0");
    CHAIN_PARAMS.push({ key: `p${n}_sample_path`, name: `P${n} Sample`,
                        type: "filepath", root: "/tmp", filter: ".wav" });
    CHAIN_PARAMS.push({ key: `p${n}_gain`, name: `P${n} Gain`,
                        type: "float", min: 0, max: 1, step: 0.01 });
    values[`p${n}_sample_path`] = "";
    values[`p${n}_gain`] = "0.5";
  }
  values.cur_pad = "1";
  HIERARCHY.levels.main.knobs = ["gain", ...FILLER];
  HIERARCHY.levels.main.params = [{ key: "gain" }, ...FILLER.map((k) => ({ key: k })),
                                  { key: "sample_path" }];
  HIERARCHY.levels.root.knobs = ["gain"];
  HIERARCHY.levels.root.params = [{ level: "main", label: "Main" }];

  openGrid();
  const slot = gotoSlotFor("sample_path");
  if (slot < 0) {
    fail("a filepath on a child level is not reachable on the grid at all");
  } else {
    feed(noteOn(slot));
    feed(click());
    const editor = ctx.activeParamEditor();
    if (editor !== "filepath") {
      fail("diving a filepath on a CHILD level opened " + JSON.stringify(editor) +
           ", expected \"filepath\" — the grid addresses the concrete key and the " +
           "editor lists the generic one, so the lookup misses and nothing opens");
    }
    feed(back());
  }
}

if (failures) process.exit(1);
console.log("PASS: editor routing — a wav_position opens the waveform, a filepath opens " +
            "the browser (child levels too), a plain number stays put, and Back returns");
'
