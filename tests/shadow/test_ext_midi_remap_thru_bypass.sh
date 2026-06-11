#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/schwung_shim.c"

# Docs (CLAUDE.md "Cable-2 Channel Remap") promise the remap is bypassed
# globally whenever any chain slot is forward=THRU, to preserve MPE
# per-note expression. Both in-place rewrite paths must honor it.

if ! rg -q 'static int any_thru_slot_active\(void\)' "$file"; then
  echo "FAIL: any_thru_slot_active() missing" >&2
  exit 1
fi

# The bypass must be an early return inside shim_remap_cable2_channels.
remap_body=$(awk '/^static void shim_remap_cable2_channels/,/^}/' "$file")
if ! grep -q 'any_thru_slot_active()' <<<"$remap_body"; then
  echo "FAIL: shim_remap_cable2_channels does not check any_thru_slot_active()" >&2
  exit 1
fi

# ...and inside shim_block_cable2_in_sh_midi (BLOCK rewrites are part of the
# same feature and equally destructive to a THRU/MPE stream).
block_body=$(awk '/^static void shim_block_cable2_in_sh_midi/,/^}/' "$file")
if ! grep -q 'any_thru_slot_active()' <<<"$block_body"; then
  echo "FAIL: shim_block_cable2_in_sh_midi does not check any_thru_slot_active()" >&2
  exit 1
fi

echo "PASS: cable-2 remap bypassed when any slot is forward=THRU"
