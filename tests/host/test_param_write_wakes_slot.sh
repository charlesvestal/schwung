#!/usr/bin/env bash
# A PARAM WRITE WAKES AN IDLE SLOT, as MIDI does.
#
# The shim skips render_block on a slot that has been silent for ~1 s and only
# probes it once every 172 frames (~0.5 s). Only MIDI woke it, so a module that
# starts making sound because of a WRITE -- Radio Garden's Play/Pause -- stayed
# silent for a random 0-0.5 s after "resume". It read as a press that did not
# land, and a second press inside that window paused it again.
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $*"; exit 1; }

grep -q 'void (\*wake_slot)(int slot);' src/host/shadow_chain_mgmt.h \
  || fail "chain_mgmt_host_t has no wake_slot callback"
grep -q '\.wake_slot = shim_wake_slot' src/schwung_shim.c \
  || fail "the shim does not hand chain_mgmt its wake_slot"
grep -A6 '^static void shim_wake_slot' src/schwung_shim.c | grep -q 'shadow_slot_idle\[slot\] = 0' \
  || fail "shim_wake_slot does not clear the slot's idle flag"

# The call must sit in the plugin-forward SET path, right after the write.
awk '/shadow_plugin_v2->set_param\(shadow_chain_slots\[slot\]\.instance,$/ {
        getline nl; if (nl ~ /key_copy, value_copy\);/) { armed = 1; n = 0; next } }
     armed { n++; if ($0 ~ /host\.wake_slot\(slot\)/) { found = 1; exit } if (n > 20) armed = 0 }
     END { exit found ? 0 : 1 }' src/host/shadow_chain_mgmt.c \
  || fail "a forwarded slot SET does not call host.wake_slot(slot)"

echo "PASS: a param write wakes an idle slot"
