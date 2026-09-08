#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module whose canvas.js FAILED TO LOAD must be tried again on the next visit
# to its page -- and must not be tried again during this one.
#
# ensureComponentWidgets sets `widgetModuleLoaded = id` before the three things
# that can still fail: resolving the module directory, reading canvas.js, and
# getting a usable widget out of it. It has to -- that is what clearWidgets() is
# paired with. But the latch was then the WHOLE answer, so a module whose script
# failed once was recorded exactly like one that had succeeded: latched, with an
# empty registry. `id === widgetModuleLoaded` refused every later attempt and
# the grid's throttle stamped the signature resolved, so its cells fell through
# to the detector and drew ordinary dials -- for the rest of the session, since
# the only thing that clears the latch is visiting a DIFFERENT module.
#
# That is the device report: "the waveform sometimes appears, and later the same
# knobs are plain dials", fixed by switching away and back.
#
# The naive repair -- never record a failure -- swaps it for the cost the
# throttle exists to prevent: a genuinely broken canvas.js re-read and re-parsed
# ~4x/sec for as long as its page is up. So four things are asserted together,
# because each of the two plausible one-line fixes breaks one of the others.

file="src/shadow/shadow_ui.js"

command -v node >/dev/null 2>&1 || { echo "FAIL: node is required" >&2; exit 1; }

tick=$(awk '/^function tickComponentWidgets\(/,/^}/' "$file")
ensure=$(awk '/^function ensureComponentWidgets\(/,/^}/' "$file")
visit=$(awk '/^function endComponentWidgetVisit\(/,/^}/' "$file")
for pair in "tickComponentWidgets:$tick" "ensureComponentWidgets:$ensure" "endComponentWidgetVisit:$visit"; do
  [ -n "${pair#*:}" ] || { echo "FAIL: ${pair%%:*} not found in $file" >&2; exit 1; }
done

retry_ticks=$(sed -n 's/^const WIDGET_RETRY_TICKS = \([0-9]*\);.*/\1/p' "$file")
[ -n "$retry_ticks" ] || { echo "FAIL: WIDGET_RETRY_TICKS not found in $file" >&2; exit 1; }

# WIRED, not merely defined. The scenario below drives the visit boundary
# directly, which cannot see whether anything on the device ever calls it -- and
# a boundary nobody calls is exactly the original bug with more code. So pin the
# call site: the frame tick ends the visit on the branch where the grid is NOT
# on screen.
if ! grep -q 'else endComponentWidgetVisit();' "$file"; then
  echo "FAIL: nothing calls endComponentWidgetVisit() when the view leaves PARAM_PAGES;" >&2
  echo "      a failed widget load would then never be retried" >&2
  exit 1
fi
if ! awk '/if \(view === VIEWS.PARAM_PAGES\) tickComponentWidgets\(\);/{f=1} f&&/else endComponentWidgetVisit\(\);/{ok=1} END{exit !ok}' "$file"; then
  echo "FAIL: endComponentWidgetVisit() is not the else of the PARAM_PAGES widget tick" >&2
  exit 1
fi

printf '%s\n%s\n%s\n' "$visit" "$ensure" "$tick" | node -e '
const fs = require("fs");
const src = fs.readFileSync(0, "utf8");
const RETRY = Number(process.argv[1]);
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

/* The real ensureComponentWidgets runs here, so the state it mutates has to be
 * declared where it can see it. Declaring rather than lifting means a rename in
 * the source fails loudly instead of silently creating a fresh global. */
const preamble = `
  let widgetModuleLoaded = "";
  let widgetLoadOk = false;
  let widgetAttemptedSig = "";
  let widgetResolvedSig = "";
  let widgetFailedVisitSig = "";
  let widgetRetryTick = 0;
`;

let slot = 0, comp = "";
let scriptLoads = 0;            /* how many times canvas.js was READ */
let canvasBroken = true;        /* the failure under test, flipped by "repair" */
const registry = [];

const HANK = { id: "hank", params: [{ viz: { kind: "custom:hank_wave" } }] };
const MODULES = { "0:synth": HANK, "1:synth": { id: "dr32", params: [{ viz: {} }] } };
const here = () => MODULES[`${slot}:${comp}`] || { id: "", params: [] };

const make = new Function(
  "paramPagesSlot", "paramPagesComponent", "getSlotParam", "componentModuleIdKey",
  "getComponentChainParams", "getModuleBasePath", "loadCanvasOverlayScript",
  "registerOverlayWidgets", "clearWidgets", "debugLog", "WIDGET_RETRY_TICKS",
  preamble + src +
  "; return { tickComponentWidgets, ensureComponentWidgets, endComponentWidgetVisit," +
  "           latch: () => widgetModuleLoaded, ok: () => widgetLoadOk };");

const api = make(
  () => slot,
  () => comp,
  () => here().id,
  () => "synth_module",
  () => here().params,
  (id) => `/modules/${id}`,
  () => {
    scriptLoads++;
    /* Exactly what the real one returns on a failed read: no overlay, an error
     * string. This is the transient the whole feature is about. */
    return canvasBroken
      ? { overlay: null, error: "failed to load canvas script" }
      : { overlay: { widgetKind: "custom:hank_wave", drawCell() {} }, error: "" };
  },
  (ov) => {
    if (!ov) return { registered: [], skipped: [] };
    registry.push(ov.widgetKind);
    return { registered: [ov.widgetKind], skipped: [] };
  },
  () => { registry.length = 0; },
  () => {},
  RETRY);

/* registerOverlayWidgets above only reports; the registry is filled here so the
 * assertions read against what the grid would actually draw from. */
const onGrid = (frames) => { for (let i = 0; i < frames; i++) api.tickComponentWidgets(); };
const leaveGrid = () => api.endComponentWidgetVisit();
const visitGrid = (s, c, frames) => { slot = s; comp = c; onGrid(frames); };

/* 1. FIRST VISIT, canvas.js broken. Nothing registers -- that part is correct
 *    and unchanged; the page falls back to the detector, which is the designed
 *    behaviour for a module whose widget cannot be drawn. */
visitGrid(0, "synth", 4);
if (api.ok())
  fail("a failed canvas.js load was recorded as settled; the module is latched " +
       "with no widget and will never be retried");
if (scriptLoads !== 1)
  fail("expected exactly 1 canvas.js read on the first visit, got " + scriptLoads);

/* 2. STAYING on the page must not re-read it. Retrying in place learns nothing
 *    -- the file is the same bytes -- and costs a read and a parse ~4x/sec. */
scriptLoads = 0;
onGrid(RETRY * 4 + 4);
if (scriptLoads !== 0)
  fail("a broken canvas.js was re-read " + scriptLoads + " times while staying on " +
       "its page; a failure must stop for the visit");

/* 3. LEAVE AND COME BACK -- still broken. One more attempt, and only one. */
leaveGrid();
scriptLoads = 0;
visitGrid(0, "synth", RETRY * 4 + 4);
if (scriptLoads !== 1)
  fail("re-entering the page made " + scriptLoads + " canvas.js reads, expected " +
       "exactly 1: a new visit retries once, it does not reopen the throttle");

/* 4. THE ACTUAL RECOVERY. The module is repaired (or its files finish
 *    installing) and the user comes back. Without the retry this is the state
 *    the device was stuck in: correct files on disk, dials on screen. */
leaveGrid();
canvasBroken = false;
visitGrid(0, "synth", 4);
if (!registry.includes("custom:hank_wave"))
  fail("a repaired module did not register its widget on the next visit");
if (!api.ok())
  fail("a successful load was not recorded as settled");

/* 5. AND SUCCESS STILL COSTS NOTHING. The visit boundary must not re-run a
 *    settled module: clearWidgets() would re-arm any widget the one-strike
 *    disable had already switched off after a throw. */
scriptLoads = 0;
leaveGrid();
visitGrid(0, "synth", RETRY * 4 + 4);
if (scriptLoads !== 0)
  fail("a settled module re-read its canvas.js " + scriptLoads + " times after a " +
       "visit boundary; that also re-arms a widget disabled for throwing");

/* 6. A module that declares NO custom kind is settled too, and reads nothing. */
scriptLoads = 0;
visitGrid(1, "synth", RETRY * 2 + 2);
if (!api.ok())
  fail("a module declaring no custom widget was left unsettled, so it is retried forever");
if (scriptLoads !== 0)
  fail("a module declaring no custom kind read a canvas.js");

console.log("PASS: a failed widget load retries per visit, once, and a settled one never does");
' "$retry_ticks"
