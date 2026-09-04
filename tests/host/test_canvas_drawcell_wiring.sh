#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET MUST NOT OUTLIVE ITS MODULE, AND THE IMPORT MUST WORK ON DEVICE.
#
# The registry is process-global and shadow_ui is long-lived, so a widget
# registered by slot 1s module would still be registered after that module was
# swapped out -- and a later module declaring the same custom: name would
# silently inherit the wrong art. resetCanvasState clears it.
#
# The import path is pinned because getting it wrong is INVISIBLE HERE and fatal
# on hardware: shadow_ui.js reaches every shared/param_pages module by the
# absolute device path /data/UserData/schwung/shared/param_pages/..., never by a
# relative one. A relative import resolves fine on a Mac checkout and cannot
# resolve on the device.
#
# Source-level pins, because shadow_ui.js cannot be imported without the host
# bindings. Weaker than unit tests; the drawing behaviour itself is covered by
# test_widget_one_strike.sh, which exercises the real registry.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the drawCell wiring checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const src = readFileSync("./src/shadow/shadow_ui.js", "utf8");

/* The import must be the absolute device path, like every other
 * shared/param_pages import in this file. */
ok(/from\s+.\/data\/UserData\/schwung\/shared\/param_pages\/widget_registry\.mjs./.test(src),
   "widget_registry is imported by its absolute device path");
ok(!/from\s+.[.][.]?\/shared\/param_pages\/widget_registry\.mjs./.test(src),
   "widget_registry is NOT imported by a relative path (fatal on device)");
for (const fn of ["registerWidget", "clearWidgets", "setWidgetLogger"]) {
  ok(new RegExp("\\b" + fn + "\\b").test(src), `shadow_ui imports ${fn}`);
}

/* Registration is guarded on BOTH the hook and the kind. */
ok(/typeof\s+\w+\.drawCell\s*===\s*"function"/.test(src),
   "a non-function drawCell is ignored rather than registered");
ok(/typeof\s+\w+\.widgetKind\s*===\s*"string"/.test(src),
   "an overlay with no widgetKind string is ignored");

/* The clear happens in the canvas teardown, not somewhere unrelated. */
const tdStart = src.indexOf("function resetCanvasState");
ok(tdStart > 0, "resetCanvasState exists");
const tdBody = src.slice(tdStart, src.indexOf("\nfunction ", tdStart + 1));
ok(/clearWidgets\s*\(\s*\)/.test(tdBody),
   "resetCanvasState clears the widget registry");

/* The logger is installed, so a disabled widget is attributable. */
ok(/setWidgetLogger\s*\(/.test(src), "shadow_ui installs the widget logger");

/* Registration sits with the overlay load, so it cannot run for a runtime that
 * failed to produce an overlay. */
const loadIdx = src.indexOf("canvasRuntime.overlay = loaded.overlay;");
ok(loadIdx > 0, "the overlay assignment is where expected");
ok(/registerWidget\s*\(/.test(src.slice(loadIdx, loadIdx + 900)),
   "registration happens right after the overlay is assigned");

process.exit(fail ? 1 : 0);
'
