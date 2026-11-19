#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Every setting on schwung-manager's Settings page must REACH the device.
#
# Four did not, each differently: Move Resample (and three other audio keys)
# was written to shadow_config.json and then overwritten by the shadow UI's
# autosave; Param View and Open With were written to shadow_config.json where
# nothing on the device reads them at all; Show MIDI reached features.json but
# had no live path, so it waited for a reboot. The page reads back its own
# write, so three of the four looked like they worked.
#
# For each key in settings-schema.json this requires ONE of the routes the
# device actually has, checked against the DEVICE source rather than restated:
#
#   live   applyShmSetting has a case for it (the shim reads SHM live)
#   adopt  shadow_ui.js adopts it from shadow_config.json: web_settings_adopt
#          or syncJsOnlySettings
#   own    a dedicated file the device reads (param_view.json)
#
# A key with none of them is a setting the web page cannot change.

fail() { echo "FAIL: $1" >&2; exit 1; }
GO=schwung-manager/main.go
UI=src/shadow/shadow_ui.js
SCHEMA=src/shared/settings-schema.json

command -v node >/dev/null 2>&1 || fail "node is required"
keys=$(node -e '
  const s = require("./'"$SCHEMA"'");
  for (const sec of s) for (const it of sec.items) console.log(it.key);')
[ -n "$keys" ] || fail "no keys in $SCHEMA"

shm_block=$(awk '/^func \(app \*App\) applyShmSetting\(/,/^}/' "$GO")
sync_block=$(awk '/^function syncJsOnlySettings\(\)/,/^}/' "$UI")
sc_key() {   # schema key -> shadow_config.json key, per settingsToShadowConfig
  local m
  m=$(awk '/^var settingsToShadowConfig/,/^}/' "$GO" | sed -n "s/^[[:space:]]*\"$1\":[[:space:]]*\"\([^\"]*\)\".*/\1/p")
  echo "${m:-$1}"
}

missing=""
for k in $keys; do
  route=""
  grep -q "case \"$k\":" <<<"$shm_block" && route=live
  sk=$(sc_key "$k")
  grep -q "configKey: \"$sk\"" src/shadow/web_settings_adopt.mjs && route=adopt
  grep -qE "c\.$sk !== undefined" <<<"$sync_block" && route=adopt
  if [ "$k" = param_view ]; then
    grep -q 'if key == "param_view"' "$GO" && grep -q "^function adoptWebParamView" "$UI" && \
      grep -q "adoptWebParamView();" "$UI" && route=own
  fi
  [ -n "$route" ] || missing="$missing $k"
done
[ -z "$missing" ] || fail "settings the web page cannot change on the device:$missing"

# Open With is persisted as the NAME the shim parses, never the index.
for n in long_press shift_vol both; do
  grep -q "\"$n\"" <<<"$(grep -n 'shadowUITriggerNames = ' "$GO")" || fail "trigger name $n missing from the manager"
  grep -q "\"$n\"" src/schwung_shim.c || fail "the shim no longer parses trigger name $n"
done
echo "PASS: every manager setting reaches the device"

# A live route is only live if the manager is ATTACHED to the control
# segment. It loses the boot race on every boot, so a direct read of app.shm
# is the nil it captured at startup: every live setting above silently waited
# for a reboot. control() attaches lazily; nothing else may touch the field.
direct=$(grep -n 'app\.shm\b' schwung-manager/*.go | grep -vE '_test\.go|^schwung-manager/shmconfig\.go:' || true)
[ -z "$direct" ] || fail "app.shm read outside control():
$direct"
ctl=$(awk '/^func \(app \*App\) control\(\)/,/^}/' schwung-manager/shmconfig.go)
grep -q "OpenShmConfig()" <<<"$ctl" || fail "control() no longer attaches lazily"
echo "PASS: the control segment is attached lazily"
