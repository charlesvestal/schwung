#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/modules/chain/dsp/chain_host.c"

# Patch saves must not squeeze chain JSON through a fixed 8 KB stack buffer:
# fat synth state (Surge ~8-16 KB; SHM transport allows 64 KB) silently
# truncated into an unparseable patch file while logging "Saved patch".
# Scope: the live v2 + master-preset region (from chain_write_file_atomic to
# EOF). The unreachable v1 path above it still has fixed buffers; it is
# slated for deletion (see docs/plans/2026-06-11-codebase-cleanup-review.md).

live_region=$(awk '/^static int chain_write_file_atomic/,0' "$file")
if [ -z "$live_region" ]; then
  echo "FAIL: chain_write_file_atomic helper missing" >&2
  exit 1
fi
if grep -q 'char final_json\[8192\]' <<<"$live_region"; then
  echo "FAIL: fixed 8KB final_json buffer in live save path (silent truncation)" >&2
  exit 1
fi
if grep -q 'char fx1\[512\]' <<<"$live_region"; then
  echo "FAIL: fixed 512B FX section buffers in master preset save (silent truncation)" >&2
  exit 1
fi

atomic=$(awk '/^static int chain_write_file_atomic\(/,/^}/' "$file")
if ! grep -Eq 'written *!=|!= *written' <<<"$atomic"; then
  echo "FAIL: chain_write_file_atomic does not check the fwrite result (disk-full passes silently)" >&2
  exit 1
fi
if ! grep -q 'rename(' <<<"$atomic"; then
  echo "FAIL: chain_write_file_atomic does not write via temp+rename" >&2
  exit 1
fi

writer=$(awk '/^static int v2_write_patch_file\(/,/^}/' "$file")
if [ -z "$writer" ]; then
  echo "FAIL: v2_write_patch_file helper missing" >&2
  exit 1
fi
if ! grep -q 'malloc' <<<"$writer"; then
  echo "FAIL: v2_write_patch_file does not size the patch buffer from the input" >&2
  exit 1
fi
if ! grep -q 'chain_write_file_atomic(' <<<"$writer"; then
  echo "FAIL: v2_write_patch_file does not use the atomic writer" >&2
  exit 1
fi

for fn in save_master_preset update_master_preset; do
  body=$(awk "/^static int ${fn}\(/,/^}/" "$file")
  if ! grep -q 'build_master_preset_json(' <<<"$body"; then
    echo "FAIL: ${fn} does not build heap-sized JSON" >&2
    exit 1
  fi
  if ! grep -q 'chain_write_file_atomic(' <<<"$body"; then
    echo "FAIL: ${fn} does not use the atomic writer" >&2
    exit 1
  fi
done

for fn in v2_save_patch v2_update_patch; do
  body=$(awk "/^static int ${fn}\(/,/^}/" "$file")
  if ! grep -q 'v2_write_patch_file(' <<<"$body"; then
    echo "FAIL: ${fn} does not route through v2_write_patch_file" >&2
    exit 1
  fi
done

echo "PASS: patch save/update heap-allocate and check write results"
