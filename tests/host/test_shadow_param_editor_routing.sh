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
                 "draw_circle","fill_circle","draw_arc","flush_display"]) {
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
      knobs: ["gain"],
      params: [{ level: "main", label: "Main" }],
    },
    /* Nine params before the divable pair, so they land on an OVERFLOW page:
     * returning to page 1 after editing something on page 2 is the bug below. */
    main: {
      label: "Main",
      knobs: ["gain", ...FILLER],
      params: [{ key: "gain" }, ...FILLER.map((k) => ({ key: k })),
               { key: "position" }, { key: "sample_path" }],
    },
  },
};
const values = { gain: "0.5", position: "0.25", sample_path: "" };
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
      for (const want of ["Knob Mapping", "LFO 1", "Save"]) {
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

if (failures) process.exit(1);
console.log("PASS: editor routing — a wav_position opens the waveform, a filepath opens " +
            "the browser, a plain number stays put, and Back returns to the grid from each");
'
