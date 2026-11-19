#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The web manager's Move Resample setting reverted to Native a few seconds
# after it was chosen. Reported from the field: "it immediately switches it
# back after a few seconds."
#
# The manager writes resample_bridge_mode (and three other audio keys) into
# shadow_config.json and nothing else. The shadow UI read them once at boot
# and the periodic autosave (~5 s, unconditional) wrote its boot-time cache
# back over the file. The fix is a three-way compare against what the shadow
# UI last persisted: web_settings_adopt.mjs. This runs it, then pins that the
# shadow UI adopts BEFORE it writes the caches and remembers after.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

fail() { echo "FAIL: $1" >&2; exit 1; }
ui="src/shadow/shadow_ui.js"

# The parser under test is the real one, lifted out of shadow_ui.js, so a
# copy cannot drift from what the device runs.
parser=$(awk '/^function parseResampleBridgeMode\(raw\)/,/^}/' "$ui")
[ -n "$parser" ] || fail "parseResampleBridgeMode is gone from shadow_ui.js"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp src/shadow/web_settings_adopt.mjs "$tmp/"
cat > "$tmp/run.mjs" <<JS
import { externalSettingChanges, settingWireValue } from "./web_settings_adopt.mjs";
${parser}
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };
const eq = (a, b, m) => { if (JSON.stringify(a) !== JSON.stringify(b)) fail(m + ": got " + JSON.stringify(a) + " want " + JSON.stringify(b)); };
const P = parseResampleBridgeMode;

/* Simulate both writers over one file. */
let file = { resample_bridge_mode: 0, link_audio_routing: false, link_audio_publish: false, latency_comp_enabled: false };
let cache = { resample_bridge_mode: 0, link_audio_routing: false, link_audio_publish: false, latency_comp_enabled: false };
let persisted = { ...cache };
let shim = { ...cache };
const adopt = () => {
  for (const c of externalSettingChanges(file, persisted, P)) {
    shim[c.configKey] = c.value; cache[c.configKey] = c.value; persisted[c.configKey] = c.value;
  }
};
const save = () => { adopt(); file = { ...file, ...cache }; persisted = { ...cache }; };

/* 1. The report: the manager writes 2 (as a JSON number), then autosave. */
file.resample_bridge_mode = 2;
save();
eq(file.resample_bridge_mode, 2, "autosave reverted the manager's Schwung Mix");
eq(shim.resample_bridge_mode, 2, "the shim never received the manager's Schwung Mix");
save(); save();
eq(file.resample_bridge_mode, 2, "a later autosave reverted it");

/* 2. Same for the three booleans. */
file.latency_comp_enabled = true; file.link_audio_publish = true; file.link_audio_routing = true;
save();
eq([file.latency_comp_enabled, file.link_audio_publish, file.link_audio_routing], [true, true, true], "manager booleans reverted");
eq([shim.latency_comp_enabled, shim.link_audio_publish, shim.link_audio_routing], [true, true, true], "manager booleans not applied");

/* 3. The other direction must survive: an ON-DEVICE change (cache moved,
 * file not yet saved) is not mistaken for an external one and undone. */
cache.resample_bridge_mode = 0; shim.resample_bridge_mode = 0;
adopt();   /* the 2 s sync runs before the save */
eq(cache.resample_bridge_mode, 0, "the sync reverted an on-device change");
save();
eq(file.resample_bridge_mode, 0, "the on-device change was not saved");

/* 4. Absent keys are not a reset; legacy 1 means Mix; never-seen adopts. */
eq(externalSettingChanges({}, {}, P), [], "an absent key was treated as a change");
eq(externalSettingChanges({ resample_bridge_mode: 1 }, { resample_bridge_mode: 2 }, P), [], "legacy 1 is not Mix");
eq(externalSettingChanges({ latency_comp_enabled: true }, {}, P).length, 1, "a never-persisted value was not adopted");
eq(externalSettingChanges({ link_audio_publish: 1 }, { link_audio_publish: true }, P), [], "numeric 1 is not true");
eq([settingWireValue(true), settingWireValue(false), settingWireValue(2)], ["1", "0", "2"], "wire values");
console.log("PASS: web-written audio settings are adopted, not reverted");
JS
node "$tmp/run.mjs"

# Wiring: adopt before the cache is written, remember after the write, and on
# load; the 2 s sync feeds its parse to the adopt.
save=$(awk '/^function saveMasterFxChainConfigOnMaster\(\)/,/^}/' "$ui")
a=$(grep -n "adoptWebAudioSettings(config);" <<<"$save" | head -1 | cut -d: -f1 || true)
w=$(grep -n "config.resample_bridge_mode = cachedResampleBridgeMode;" <<<"$save" | head -1 | cut -d: -f1 || true)
[ -n "$a" ] && [ -n "$w" ] && [ "$a" -lt "$w" ] || \
  fail "saveMasterFxChainConfig must adopt web changes BEFORE writing the caches"
grep -A1 "host_write_file(configPath, JSON.stringify(config, null, 2));" <<<"$save" | \
  grep -q "rememberPersistedWebAudio();" || fail "save does not remember what it wrote"
load=$(awk '/^function loadMasterFxChainFromConfigOnMaster\(\)/,/^}/' "$ui")
grep -q "rememberPersistedWebAudio();" <<<"$load" || fail "load does not remember what it read"
grep -q "if (syncedConfig) adoptWebAudioSettings(syncedConfig);" "$ui" || \
  fail "the periodic config sync no longer adopts web changes"

# Every key the manager maps into shadow_config.json and the shadow UI writes
# from a cache must be in the adopt table, or it has this bug.
for k in resample_bridge_mode link_audio_routing link_audio_publish latency_comp_enabled; do
  grep -q "configKey: \"$k\"" src/shadow/web_settings_adopt.mjs || fail "$k is not adopted"
done
echo "PASS: shadow UI wiring"
