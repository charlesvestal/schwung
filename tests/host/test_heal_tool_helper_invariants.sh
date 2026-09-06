#!/usr/bin/env bash
# Source invariants for schwung-heal's standalone-tool helper install.
#
# This is a setuid-root binary that cannot be exercised here (it setuid(0)s and
# bails when it is not root), and the two properties below are structural
# rather than computational, so a unit cannot reach them:
#
#   1. A tool's failure must not feed the exit code, because the exit code
#      gates --reboot and that reboot belongs to OUR shim/entrypoint mirror.
#      Folding them together lets a third-party tool's bad stage turn a
#      /system/repair into "shim mirrored, device never rebooted".
#
#   2. The tool path is the one place this binary works on names it did not
#      compile in — <id> and bin/ are ableton-writable — so every component is
#      resolved O_NOFOLLOW and every write goes through a descriptor. A
#      path-based copy would follow a planted symlink and steer a root-owned
#      04755 write anywhere on the filesystem.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src/schwung-heal.c"
HDR="$ROOT/src/host/heal_tool_id.h"
fail=0

note() { echo "  ok:  $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

[ -f "$SRC" ] || { echo "missing $SRC"; exit 1; }
[ -f "$HDR" ] || { echo "missing $HDR"; exit 1; }

# --- 1. the reboot gate -----------------------------------------------------
# Any line that calls install_tool_helpers and also mentions rc is the
# regression: `if (install_tool_helpers() != 0) rc = 2;` and friends.
if grep -n 'install_tool_helpers' "$SRC" | grep -q '\brc\b'; then
    bad "install_tool_helpers() result feeds rc — that gates --reboot"
else
    note "a tool helper failure does not feed rc (so it cannot suppress --reboot)"
fi

if grep -q 'helper_failures' "$SRC"; then
    note "helper failures are counted and reported separately"
else
    bad "expected a separate helper failure count that is reported, not returned"
fi

# The gate itself must still exist for our own mirrors.
if grep -q 'do_reboot && rc == 0' "$SRC"; then
    note "--reboot is still gated on rc for the shim/entrypoint mirror"
else
    bad "the do_reboot/rc gate moved — re-check what can now suppress a reboot"
fi

# --- 2. symlink-proof resolution -------------------------------------------
helper="$(awk '/^static int install_one_tool_helper/,/^}/' "$SRC")"
[ -n "$helper" ] || { echo "  FAIL: install_one_tool_helper not found"; exit 1; }

nofollow_opens=$(printf '%s\n' "$helper" | grep -c 'O_NOFOLLOW')
if [ "$nofollow_opens" -ge 4 ]; then
    note "every component is opened O_NOFOLLOW ($nofollow_opens opens)"
else
    bad "expected >=4 O_NOFOLLOW opens (<id>, bin, the stage, the tmp), found $nofollow_opens"
fi

for want in 'openat' 'renameat' 'unlinkat' 'O_EXCL' 'O_DIRECTORY' 'O_NONBLOCK' 'fchmod'; do
    if printf '%s\n' "$helper" | grep -q "$want"; then
        note "uses $want"
    else
        bad "install_one_tool_helper no longer uses $want"
    fi
done

# A path-based rename/unlink inside the helper means a component is being
# re-resolved by name, which is exactly what the descriptors are avoiding.
if printf '%s\n' "$helper" | grep -Eq '[^a-z_](rename|unlink)[[:space:]]*\('; then
    bad "path-based rename()/unlink() in the helper — use renameat/unlinkat"
else
    note "no path-based rename()/unlink() re-resolves a component by name"
fi

# O_NONBLOCK is what keeps a FIFO left at heal.new from wedging boot:
# shim-entrypoint.sh runs heal before the LD_PRELOAD exec.
if printf '%s\n' "$helper" | grep -q 'S_ISREG'; then
    note "the stage is still required to be a regular file"
else
    bad "the S_ISREG check on the stage is gone"
fi

# --- 3. the id filter lives in the header, once ----------------------------
if grep -q 'heal_tool_id_is_safe' "$SRC"; then
    note "the id filter is called from the shared header"
else
    bad "schwung-heal.c no longer uses heal_tool_id_is_safe()"
fi

# A restated character class in the .c is a second copy of the policy that
# tests/host cannot see.
if awk '/^static int install_tool_helpers/,/^}/' "$SRC" | grep -q "'z'"; then
    bad "the id character class is restated in schwung-heal.c — keep it in the header"
else
    note "the character class is not restated in the .c"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: schwung-heal tool helper invariants"
else
    echo "FAIL: schwung-heal tool helper invariants"
    exit 1
fi
