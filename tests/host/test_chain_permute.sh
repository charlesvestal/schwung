#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_chain_permute"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/modules/chain/dsp -Isrc/host \
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
    if ! command grep -qE "PERM_(FIELD|OWNED)\(inst->$f[,)]" "$src"; then
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
for f in $(command grep -oE 'PERM_(FIELD|OWNED)\(inst->[a-z_0-9]+' "$src" \
           | sed -E 's/.*inst->//' | sort -u); do
  if ! printf '%s\n' "$struct_body" | command grep -q "[ *(]$f\[MAX_"; then
    echo "FAIL: chain_reorder.c permutes '$f', which is not a per-position field" >&2
    exit 1
  fi
done

# --------------------------------------------------------------------------
# THE CLASSIFICATION, which is the one that reached hardware.
#
# A per-position array is either a VALUE array (vacating a position zeroes its
# bytes) or an OWNED-BUFFER array -- a pointer to a block allocated once per
# position and NEVER NULL. The second kind must be ROTATED: zeroing the pointer
# leaves a null that the next module load writes its param table through, which
# is a SIGSEGV on the SCHED_FIFO SPI callback (seen loading a MIDI FX in front
# of an existing one), and the shift also leaks the allocation it overwrites.
#
# The two kinds are indistinguishable to chain_permute.h, so the classification
# is written by hand in chain_reorder.c -- and therefore must not be TRUSTED.
# The set of owned buffers is not a matter of opinion: it is exactly what
# chain_alloc_position_storage allocates. Derive it from there and compare.
# --------------------------------------------------------------------------
owned=$(awk '/^static inline int chain_alloc_position_storage/,/^\}/' "$hdr" \
  | command grep -oE 'inst->[a-z_0-9]+\[i\] *= *\(' \
  | sed -E 's/inst->([a-z_0-9]+).*/\1/' | sort -u)

if [ -z "$owned" ]; then
  echo "FAIL: could not read the owned-buffer set out of chain_alloc_position_storage" >&2
  exit 1
fi

for f in $owned; do
  if ! command grep -q "PERM_OWNED(inst->$f," "$src"; then
    echo "FAIL: '$f' is allocated per position by chain_alloc_position_storage but is" >&2
    echo "      NOT registered as PERM_OWNED in chain_reorder.c. Vacating a position" >&2
    echo "      would zero its POINTER instead of its contents -- a NULL deref inside" >&2
    echo "      the next module load, on the audio thread." >&2
    exit 1
  fi
done

# ...and nothing else may claim to be one: rotating a value array would treat
# its bytes as an address.
for f in $(command grep -oE 'PERM_OWNED\(inst->[a-z_0-9]+' "$src" \
           | sed -E 's/.*inst->//' | sort -u); do
  if ! printf '%s\n' "$owned" | command grep -qx "$f"; then
    echo "FAIL: chain_reorder.c registers '$f' as an owned buffer, but" >&2
    echo "      chain_alloc_position_storage never allocates one for it" >&2
    exit 1
  fi
done

echo "PASS: chain permute enumeration — every per-position field of chain_instance_t is"
echo "      permuted, and every owned buffer is classified from the allocator rather"
echo "      than by hand"
