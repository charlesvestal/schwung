#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

for file in src/modules/chain/dsp/chain_host.c src/host/shadow_chain_mgmt.c src/host/shadow_midi.c src/schwung_shim.c; do
  grep -q touch_observe "$file" || { echo "FAIL: touch_observe missing from $file"; exit 1; }
done
grep -q 'note > 9 || note == 8' src/host/shadow_midi.c || { echo "FAIL: touch range is not bounded"; exit 1; }
grep -q 'td1 == 9.*shadow_ui_midi_shm' src/schwung_shim.c || { echo "FAIL: jog touch is not relayed to UI"; exit 1; }
echo "PASS: touch_observe is opt-in, bounded, and jog reaches the UI"
