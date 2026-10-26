#!/usr/bin/env bash
# AN EMPTY TRACK'S SEND A/B SURVIVE LEAVING AND RE-ENTERING THE SET.
#
# Every slot has a chain instance once the chain is up, and the instance is what
# the mix drains: the UI writes `buses:main_send<N>` straight to it. The shim's
# own `empty_send` mirror was never written, so the save stored 0, and the slot
# clear on a set change zeroes the instance's sends in any case. Hardware: Send
# A to 100% on a track with no module, leave the set, come back, Send A at 0%.
set -euo pipefail
cd "$(dirname "$0")/../.."

# 1. The shim half: the save/restore key reads and writes THROUGH to the chain.
mgmt=src/host/shadow_chain_mgmt.c
set_blk=$(awk '/^int shadow_handle_slot_param_set\(/,/slot:pan/' "$mgmt")
echo "$set_blk" | grep -q 'shadow_plugin_v2->set_param(shadow_chain_slots\[slot\].instance, ck, cv)' \
  || { echo "FAIL: slot:empty_send<N> does not write through to the chain instance"; exit 1; }
get_blk=$(awk '/^int shadow_handle_slot_param_get\(/,/slot:volume/' "$mgmt")
echo "$get_blk" | grep -q 'shadow_plugin_v2->get_param(shadow_chain_slots\[slot\].instance' \
  || { echo "FAIL: slot:empty_send<N> does not read the chain instance's level"; exit 1; }

# 2. Boot: the sends are put on the instance before a slot's own patch loads.
boot=$(awk '/Check for autosave file/{exit} {print}' "$mgmt" | grep -c 'empty_send\[sb\]' || true)
[ "$boot" -ge 1 ] || { echo "FAIL: boot never stages the saved empty-slot sends"; exit 1; }

# 3. The UI half: a set change writes them AGAIN after the slot clear, for a
#    slot with no saved patch (a clear zeroes the chain's sends).
ui=src/shadow/shadow_ui.js
start=$(grep -n 'if (flags & SHADOW_UI_FLAG_SET_CHANGED) setChange: {' "$ui" | head -1 | cut -d: -f1)
clear_pass=$(awk -v s="$start" 'NR>s && /clearing all slots/ {print NR; exit}' "$ui")
reapply=$(awk -v s="$start" 'NR>s && /if \(!hasState\) applySavedEmptySends\(i\);/ {print NR; exit}' "$ui")
[ -n "$clear_pass" ] && [ -n "$reapply" ] && [ "$clear_pass" -lt "$reapply" ] \
  || { echo "FAIL: the saved sends are not re-applied after the slot clear"; exit 1; }

# 4. The per-set config parse, run through the real shadow_set_pages.c.
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
mkdir -p "$work/sys"
cat > "$work/sys/xattr.h" <<'H'
#include <sys/types.h>
static inline ssize_t getxattr(const char *p, const char *n, void *v, size_t s) { (void)p; (void)n; (void)v; (void)s; return -1; }
static inline int setxattr(const char *p, const char *n, const void *v, size_t s, int f) { (void)p; (void)n; (void)v; (void)s; (void)f; return -1; }
H
bin=build/tests/test_empty_slot_sends_persist
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wno-unused-function -I"$work" -Isrc -Isrc/host \
  tests/host/test_empty_slot_sends_persist.c src/host/shadow_set_pages.c -o "$bin"
"$bin"
echo "PASS: an empty track's sends are saved, restored on a set change and on boot"
