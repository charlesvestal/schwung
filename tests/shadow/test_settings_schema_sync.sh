#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

# shadow_ui.js:880 — "The canonical schema is also in shared/settings-schema.json
# for the schwung-manager web UI. Keep both in sync when adding settings."
# This test enforces that: every editable (non-action) key in the on-device
# GLOBAL_SETTINGS_SECTIONS must exist in settings-schema.json, or the web UI
# silently cannot show/edit it. Device-only action sections (updates, help)
# are exempt.

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const m = src.match(/const GLOBAL_SETTINGS_SECTIONS = (\[[\s\S]*?\n\]);/);
if (!m) { console.error("FAIL: GLOBAL_SETTINGS_SECTIONS not found"); process.exit(1); }
const sections = new Function("return " + m[1])();
const schema = JSON.parse(fs.readFileSync("src/shared/settings-schema.json", "utf8"));

const schemaKeys = new Set();
for (const s of schema) for (const it of (s.items || [])) schemaKeys.add(it.key);

const deviceOnlySections = new Set(["updates", "help"]);
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
'
