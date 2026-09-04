#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# install.sh REWRITES /data/UserData/schwung/config/features.json from scratch on
# every deploy, so a key it does not carry across is silently reset to its
# default. That is not a hypothetical: seven keys written by features_json_set()
# in src/shadow/shadow_ui.c were being reset on every update, which presents as
# "the update reset my settings" and so never gets filed as an installer bug.
#
# The fix inverts the rule -- the installer owns a fixed set of keys and
# preserves everything else verbatim -- and this test runs the extracted merge
# against a realistic file rather than reading it.

fails=0
say_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ---- 1. the merge loop, lifted from install.sh and run for real -----------
#
# Lifted rather than restated: a copy of the logic here could drift from the
# shipped one and go on passing. The slice is delimited by the two markers the
# script itself carries.
# awk, not `grep -n | head -1`: head exits after the first line, grep takes
# SIGPIPE, and `set -o pipefail` turns that into a failed assignment. It is
# timing-dependent — the same shape passed on macOS for days and then failed in
# CI on Linux (see test_sampler_io_politeness.sh, which is where it bit).
first_line_matching() {
    awk -v pat="$1" '!n && index($0, pat) == 1 { n = NR } END { if (n) print n }' "$2"
}
start=$(first_line_matching 'managed_keys=' scripts/install.sh)
end=$(first_line_matching '# Build features.json content' scripts/install.sh)
if [ -z "$start" ] || [ -z "$end" ] || [ "$start" -ge "$end" ]; then
    echo "FAIL: could not slice the carry-over block out of scripts/install.sh"
    exit 1
fi
merge_block=$(sed -n "${start},$((end - 1))p" scripts/install.sh)

# A features.json shaped exactly as features_json_set() leaves one: the
# installer-managed keys, plus every key that only the device ever writes.
existing_features='{
  "shadow_ui_enabled": true,
  "link_audio_enabled": true,
  "display_mirror_enabled": false,
  "ext_midi_remap_enabled": true,
  "shadow_ui_trigger": "shift_vol",
  "stay_in_shadow": false,
  "set_pages_enabled": true,
  "midi_indicator_enabled": true,
  "skipback_require_volume": true,
  "skipback_seconds": 120,
  "recall_quantize": "bar",
  "metronome_mode": "on",
  "metronome_level": 75,
  "save_stems": "both"
}'

carried_features=""
eval "$merge_block"

# ---- 2. every device-written key survives, with its VALUE ---------------
#
# The value matters as much as the key: carrying "skipback_seconds" across but
# resetting it to 30 is the same bug wearing the key's name.
for pair in \
    'set_pages_enabled:true' \
    'midi_indicator_enabled:true' \
    'skipback_require_volume:true' \
    'skipback_seconds:120' \
    'recall_quantize:"bar"' \
    'metronome_mode:"on"' \
    'metronome_level:75' \
    'save_stems:"both"'
do
    key="${pair%%:*}"
    val="${pair#*:}"
    case "$carried_features" in
        *"\"$key\": $val"*) ;;
        *) say_fail "$key is not carried across with its value ($val) — it would reset on every deploy" ;;
    esac
done

# ---- 3. the installer-managed keys are NOT duplicated -------------------
#
# They are emitted by the block above the merge. Carrying them again produces a
# JSON object with the key twice, and which one wins is up to the parser.
for key in shadow_ui_enabled link_audio_enabled display_mirror_enabled \
           ext_midi_remap_enabled shadow_ui_trigger stay_in_shadow; do
    case "$carried_features" in
        *"\"$key\""*) say_fail "$key was carried across as well as emitted — the key would appear twice" ;;
    esac
done

# The legacy bool the trigger migration reads must not be resurrected either:
# writing it back keeps a migrated device migrating forever.
existing_features='{
  "shadow_ui_enabled": true,
  "long_press_shadow": false,
  "save_stems": "stems"
}'
carried_features=""
eval "$merge_block"
case "$carried_features" in
    *long_press_shadow*) say_fail "the legacy long_press_shadow key is carried across" ;;
esac
case "$carried_features" in
    *'"save_stems": "stems"'*) ;;
    *) say_fail "save_stems lost when the file also holds the legacy trigger key" ;;
esac

# ---- 4. an absent or empty file produces nothing ------------------------
#
# A first install has no file to read. Emitting a stray comma there would write
# invalid JSON and take every setting with it.
existing_features=""
carried_features=""
eval "$merge_block"
if [ -n "$carried_features" ]; then
    say_fail "a missing features.json produced a non-empty carry-over: '$carried_features'"
fi

# ---- 5. the result parses as JSON --------------------------------------
#
# The carry-over is glued on after the last managed key, so it must start with
# its own comma and never leave a trailing one.
existing_features='{
  "shadow_ui_enabled": true,
  "recall_quantize": "2bars",
  "save_stems": "both"
}'
carried_features=""
eval "$merge_block"
built="{
  \"shadow_ui_enabled\": true,
  \"link_audio_enabled\": true,
  \"display_mirror_enabled\": false,
  \"ext_midi_remap_enabled\": true,
  \"shadow_ui_trigger\": \"both\",
  \"stay_in_shadow\": true$carried_features
}"
if ! echo "$built" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    say_fail "the assembled features.json does not parse:
$built"
fi

# ---- 6. non-vacuity: every key shadow_ui.c writes is covered ------------
#
# The whole point is that a NEW setting is carried across without anyone
# remembering to add it here. This asserts the merge covers the real key list
# rather than the sample above — so adding a features_json_set() call cannot
# reintroduce the bug.
keys=$(grep -o 'features_json_set("[a-z_]*"' src/shadow/shadow_ui.c | sed 's/.*("//; s/"//')
if [ -z "$keys" ]; then
    say_fail "found no features_json_set() keys in src/shadow/shadow_ui.c"
fi
managed="shadow_ui_enabled link_audio_enabled display_mirror_enabled ext_midi_remap_enabled shadow_ui_trigger stay_in_shadow long_press_shadow"
covered=0
# `k`, not `key`: the merge block runs in THIS scope and has its own loop
# variables. That collision is why this loop first reported eight failures
# against an empty key name -- the eval had overwritten it before the check
# ran, and the block's variables are _-prefixed now for the same reason.
for k in $keys; do
    case " $managed " in *" $k "*) continue ;; esac
    existing_features="{
  \"$k\": \"probe\"
}"
    carried_features=""
    eval "$merge_block"
    case "$carried_features" in
        *"\"$k\": \"probe\""*) covered=$((covered + 1)) ;;
        *) say_fail "$k is written to features.json by shadow_ui.c but is not carried across a deploy" ;;
    esac
done

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "PASS: features.json survives a deploy — the installer owns 6 keys and preserves everything else verbatim ($covered device-written keys checked, values intact, no duplicates, no legacy key resurrected, empty file safe, result parses)"
