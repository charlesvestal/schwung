#!/usr/bin/env bash
# Source pin: a slot's cached capabilities.default_forward_channel is refreshed
# on EVERY path that can change its synth, and refreshed unconditionally.
#
# Auto forwarding (shadow_midi.c) reads the cache, never the module, because it
# runs on the SPI callback. The param handler refreshed it after synth:module
# and load_patch but NOT after load_file -- the path shadow_ui uses to restore
# a set's slots at boot and on every set switch. MiniJV (default channel 1)
# then came up on Auto forwarding the receive channel and ignored every note,
# with the slot loaded, unmuted and rendering silence (hardware, 2026-09-30).
# And a module declaring NO preference left the previous module's value, so a
# slot could inherit a channel it never asked for.
set -u
SRC="$(dirname "$0")/../../src/host/shadow_chain_mgmt.c"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
[ -f "$SRC" ] || { echo "FAIL: cannot find $SRC" >&2; exit 1; }
code() { grep -vE '^\s*(/\*|\*|//)' "$SRC"; }

# One place reads the value; nothing else queries it.
q=$(code | grep -c '"synth:default_forward_channel"')
[ "$q" -eq 1 ] || fail "synth:default_forward_channel is queried in $q places; only shadow_slot_refresh_default_fwd may"

# The helper assigns unconditionally (-1 when the module declares nothing).
body=$(awk '/^static void shadow_slot_refresh_default_fwd\(int slot\)/,/^}/' "$SRC")
echo "$body" | grep -q 'int fwd = -1;' || fail "refresh must default to -1 (clear), not keep the old value"
echo "$body" | grep -q 'default_forward_channel = fwd;' || fail "refresh must assign unconditionally"

# Every synth-changing param path calls it.
for key in 'synth:module' 'load_file' 'load_patch'; do
    # The branch that tests exactly this key and opens a block ("== 0) {"),
    # not an earlier compound condition that merely mentions it.
    blk=$(awk -v k="$key" 'index($0, "strcmp(key_copy, \"" k "\") == 0) {") || index($0, "strcmp(key_copy, \"" k "\") == 0 ||") && $0 ~ /load_patch/ {on=1} on {print; n++} n > 40 {exit}' "$SRC")
    [ -n "$blk" ] || { fail "no param branch for $key"; continue; }
    echo "$blk" | grep -q 'shadow_slot_refresh_default_fwd(slot)' || fail "the $key branch does not refresh the default forward channel"
done

# Both boot-restore paths too.
boot=$(code | grep -c 'shadow_slot_refresh_default_fwd(i)')
[ "$boot" -ge 2 ] || fail "boot restore paths refresh in $boot places, want 2"

[ "$fails" -eq 0 ] && echo "PASS: default forward channel refreshed on every load" || exit 1
