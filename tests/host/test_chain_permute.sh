#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_chain_permute"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/modules/chain/dsp \
  tests/host/test_chain_permute.c \
  -o "$bin"

"$bin"

# --------------------------------------------------------------------------
# THE ENUMERATION. chain_reorder.c lists every per-position array by hand, and
# a field left out of that list keeps the value belonging to whatever module
# USED to be at the index -- a bypass flag that follows the position instead of
# the module, or param metadata that makes one module's knob write another
# module's parameter. Nothing in C catches that, so it is caught here: every
# [MAX_AUDIO_FX] / [MAX_MIDI_FX] member of chain_instance_t must appear in the
# matching collector.
# --------------------------------------------------------------------------
hdr=src/modules/chain/dsp/chain_internal.h
src=src/modules/chain/dsp/chain_reorder.c

# The struct body only -- patch_info_t has per-position arrays too, and those
# are the SAVED library rather than the live chain, so they must not be listed.
struct_body=$(awk '/^typedef struct chain_instance \{/,/^\} chain_instance_t;/' "$hdr")

missing=""
for cap in MAX_AUDIO_FX MAX_MIDI_FX; do
  fields=$(printf '%s\n' "$struct_body" \
    | command grep "\[$cap\]" \
    | sed -e 's/\/\*.*//' \
    | sed -E 's/.*[ *(]([a-z_0-9]+)\['"$cap"'\].*/\1/' \
    | sort -u)
  for f in $fields; do
    if ! command grep -q "inst->$f)" "$src"; then
      missing="$missing $cap:$f"
    fi
  done
done

if [ -n "$missing" ]; then
  echo "FAIL: per-position fields not permuted by chain_reorder.c -$missing" >&2
  echo "      A field left out keeps the value of the module that USED to be at" >&2
  echo "      that index. Add it to chain_perm_collect_fx / _collect_midi_fx." >&2
  exit 1
fi

# ...and the reverse: the collectors must not claim a field that is gone.
for f in $(command grep -oE 'inst->[a-z_0-9]+\)' "$src" | sed -e 's/inst->//' -e 's/)//' | sort -u); do
  if ! printf '%s\n' "$struct_body" | command grep -q "[ *(]$f\[MAX_"; then
    echo "FAIL: chain_reorder.c permutes '$f', which is not a per-position field" >&2
    exit 1
  fi
done

echo "PASS: chain permute enumeration — every per-position field of chain_instance_t is permuted"
