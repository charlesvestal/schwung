#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# DUPLICATING A SET CARRIES ALL OF ITS SCHWUNG STATE.
#
# The copy was a list of what to copy -- slot_N, master_fx_N, controls.json,
# shadow_chain_config.json -- and every per-set file added after it was left
# behind in silence: chance_N.txt, lanes_N.json, scenes.json, send_fx_*.json,
# send_levels.json. It is a list of what NOT to copy now
# (src/shadow/set_state_copy.mjs). This RUNS the command against a real
# directory, so it fails on a file the copy drops, not on a missing line.

fail() { echo "FAIL: $1"; exit 1; }

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
src="$work/set_state/aaaa-src"; dst="$work/set_state/bbbb-dup"
mkdir -p "$src/snapshot" "$dst"
for f in slot_0.json slot_3.json master_fx_7.json send_fx_1_7.json send_levels.json \
         chance_1.txt lanes_2.json scenes.json controls.json shadow_chain_config.json \
         some_future_state.json; do
  echo "$f" > "$src/$f"
done
echo "snap" > "$src/snapshot/slot_0.json"
echo "aaaa-src-parent" > "$src/copy_source.txt"
echo "aaaa-src" > "$dst/copy_source.txt"

cmd=$(node --input-type=module -e "
import { setStateCopyCommand } from './src/shadow/set_state_copy.mjs';
process.stdout.write(setStateCopyCommand('$src', '$dst') ?? 'NULL');
")
[ "$cmd" != "NULL" ] || fail "setStateCopyCommand refused a plain set_state path"
case "$cmd" in "sh "*) ;; *) fail "the command must start with 'sh ' (host_system_cmd's allowlist)";; esac
/bin/sh -c "$cmd" || fail "the copy command exited non-zero"

for f in slot_0.json slot_3.json master_fx_7.json send_fx_1_7.json send_levels.json \
         chance_1.txt lanes_2.json scenes.json controls.json shadow_chain_config.json \
         some_future_state.json; do
  [ -f "$dst/$f" ] || fail "a duplicate dropped $f"
  [ "$(cat "$dst/$f")" = "$f" ] || fail "$f was not copied verbatim"
done
[ ! -e "$dst/snapshot" ] || fail "snapshot/ was copied (it is re-seeded per set, never carried)"
[ "$(cat "$dst/copy_source.txt")" = "aaaa-src" ] || fail "the source's copy_source.txt overwrote the duplicate's"

# A path the command cannot quote is refused, never interpolated.
bad=$(node --input-type=module -e "
import { setStateCopyCommand } from './src/shadow/set_state_copy.mjs';
process.stdout.write(String(setStateCopyCommand(\"/x/a'; rm -rf /\", '/y')));
")
[ "$bad" = "null" ] || fail "a path with a quote was not refused"

# The seeding path USES it, and no longer enumerates files by name.
ui=src/shadow/shadow_ui.js
grep -q "setStateCopyCommand(copySourceDir, newDir)" "$ui" || fail "the duplicate path does not use setStateCopyCommand"
awk '/SET_CHANGED: duplicated set, copying from/{p=1} p&&/New set \(or a failed copy\)/{exit} p' "$ui" > "$work/dup_branch.js"
[ -s "$work/dup_branch.js" ] || fail "cannot find the duplicate branch in shadow_ui.js"
if grep -q 'copySourceDir + "/slot_\|copySourceDir + "/master_fx_' "$work/dup_branch.js"; then
  fail "the duplicate branch still copies files by name"
fi

echo "PASS: a duplicated set carries every per-set file except snapshot/ and copy_source.txt"
