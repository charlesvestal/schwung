#!/usr/bin/env bash
# The stock Move binary must survive re-hooking after a stock firmware update.
#
# A stock update rewrites /opt/move: /opt/move/Move is stock again and there is
# no MoveOriginal. post-update.sh used to run schwung-heal FIRST; heal mirrored
# shim-entrypoint.sh over /opt/move/Move (deleting the only stock copy), and the
# backup step after it saved the entrypoint script as MoveOriginal. The device
# then has no Move app (seen 2026-11-28; recovered from /proc/<pid>/exe).
#
# Part 1 is structural (heal is setuid-root and cannot run here). Part 2 runs
# the post-update.sh ordering against a fake /opt/move.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/src/schwung-heal.c"
POST="$ROOT/scripts/post-update.sh"
fail=0

note() { echo "  ok:  $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

# --- 1. heal only mirrors the entrypoint over a saved stock binary -----------
mirror="$(awk '/Entrypoint — perms 0755/,/entrypoint mirrored/' "$SRC")"
if printf '%s\n' "$mirror" | grep -q 'stock_binary_saved'; then
    note "the entrypoint mirror is gated on stock_binary_saved()"
else
    bad "the entrypoint mirror over /opt/move/Move is not gated on a saved MoveOriginal"
fi
if printf '%s\n' "$mirror" | grep -q 'rc = 2'; then
    note "a refused mirror sets rc, so --reboot does not reboot into it"
else
    bad "a refused entrypoint mirror must set rc (it gates --reboot)"
fi
saved="$(awk '/^static int stock_binary_saved/,/^}/' "$SRC")"
if printf '%s\n' "$saved" | grep -q '/opt/move/MoveOriginal' \
   && printf '%s\n' "$saved" | grep -q 'shim-entrypoint.sh'; then
    note "stock_binary_saved() checks MoveOriginal exists and is not the entrypoint"
else
    bad "stock_binary_saved() must check MoveOriginal and compare it to the entrypoint"
fi

# --- 2. post-update.sh saves the stock binary before heal runs -----------------
backup_line=$(grep -n 'mv /opt/move/Move /opt/move/MoveOriginal' "$POST" | head -n 1 | cut -d: -f1)
heal_line=$(grep -n '"\$BASE/bin/schwung-heal"' "$POST" | head -n 1 | cut -d: -f1)
if [ -n "$backup_line" ] && [ -n "$heal_line" ] && [ "$backup_line" -lt "$heal_line" ]; then
    note "post-update.sh saves the stock binary (line $backup_line) before heal (line $heal_line)"
else
    bad "post-update.sh must save /opt/move/Move as MoveOriginal before running schwung-heal"
fi

# Run the backup block itself against a fake /opt/move: a stock-updated device,
# and one whose /opt/move/Move is already the entrypoint (must not be "saved").
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
block="$(awk '/^# --- Save the stock Move binary/,/^fi$/' "$POST" | sed "s#/opt/move#$tmp/opt#g")"
[ -n "$block" ] || { echo "  FAIL: backup block not found in post-update.sh"; exit 1; }
mkdir -p "$tmp/opt" "$tmp/base"
printf '#!/usr/bin/env bash\n# entrypoint\n' > "$tmp/base/shim-entrypoint.sh"

printf 'STOCK-ELF' > "$tmp/opt/Move"
BASE="$tmp/base" bash -c "$block"
if [ "$(cat "$tmp/opt/MoveOriginal" 2>/dev/null)" = "STOCK-ELF" ]; then
    note "after a stock update, the stock binary is saved as MoveOriginal"
else
    bad "after a stock update, MoveOriginal is not the stock binary"
fi

rm -f "$tmp/opt/MoveOriginal"
cp "$tmp/base/shim-entrypoint.sh" "$tmp/opt/Move"
BASE="$tmp/base" bash -c "$block"
if [ ! -e "$tmp/opt/MoveOriginal" ]; then
    note "an entrypoint at /opt/move/Move is never saved as MoveOriginal"
else
    bad "the entrypoint script was saved as MoveOriginal"
fi

exit $fail
