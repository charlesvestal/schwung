#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

# "The canonical schema is also in shared/settings-schema.json for the
# schwung-manager web UI. Keep both in sync when adding settings."
# This test enforces that: every editable (non-action) key in the on-device
# Global Settings declaration must exist in settings-schema.json, or the web UI
# silently cannot show/edit it. Device-only sections (updates) are exempt.
#
# The source of truth MOVED. It used to be GLOBAL_SETTINGS_SECTIONS, a literal
# in shadow_ui.js scraped out with a regex; Global Settings is a synthesised
# module contract now, so the declaration is imported instead of parsed.

node -e '
const fs = require("fs");
const { pathToFileURL } = require("url");
import(pathToFileURL(process.cwd() + "/src/shadow/shadow_ui_global_grid.mjs").href).then((G) => {
const sections = G.GLOBAL_SECTIONS.map((s) => ({ id: s.id, items: s.params }));
const schema = JSON.parse(fs.readFileSync("src/shared/settings-schema.json", "utf8"));

const schemaKeys = new Set();
for (const s of schema) for (const it of (s.items || [])) schemaKeys.add(it.key);

const deviceOnlySections = new Set(["updates"]);
/* analytics_enabled persists via opt-in/opt-out flag files
 * (src/host/analytics.c), not features.json/shadow_config.json — the
 * schema-driven manager config cannot write it, so a schema entry would be
 * a silently broken web toggle. Device-only by design. */
const deviceOnlyKeys = new Set(["analytics_enabled"]);
const missing = [];
for (const s of sections) {
  if (deviceOnlySections.has(s.id)) continue;
  for (const it of (s.items || [])) {
    if (it.type === "action") continue;
    if (deviceOnlyKeys.has(it.key)) continue;
    if (!schemaKeys.has(it.key)) missing.push(s.id + "/" + it.key);
  }
}
if (missing.length) {
  console.error("FAIL: settings-schema.json missing keys: " + missing.join(", "));
  process.exit(1);
}
console.log("PASS: settings-schema.json covers all editable device settings");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
