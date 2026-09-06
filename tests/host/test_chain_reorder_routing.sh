#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves, the same split test_chain_knob_target_lookup.sh uses:
#
#   RUN   the real chain_reorder.c against a real chain_instance_t, with the
#         real chain_mod.c and chain_params.c, and assert on what the user
#         hears -- a modulation base, an orphaned LFO, a knob on fx6.
#         See the header of tests/host/test_chain_reorder_routing.c.
#
#   PIN   the two things the run half cannot reach: the shipped per-position
#         unloaders (one of them lives in the TU that dlopens plugins and
#         cannot be compiled natively), and the completeness of the retarget.

fail() { echo "FAIL: $1" >&2; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_reorder_routing"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-reorder-routing.XXXXXX")"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

# -Wno-sign-compare: chain_params.c has pre-existing int/size_t comparisons
# that are not this test's business to fix.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_reorder_routing.c \
  src/modules/chain/dsp/chain_reorder.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half
#
# The test fixture supplies its own v2_unload_audio_fx_slot /
# v2_unload_midi_fx_slot, because the audio one lives in chain_host.c. A
# fixture that dropped a position's modulation entries when production did not
# would make the run half a test of the fixture. So require the shipped
# unloaders to do the same thing: name their OWN position and clear its
# modulation entries WITHOUT restoring the base (the module is leaving; writing
# a base back into a plugin that is about to be destroyed is at best wasted and
# at worst a write through a stale instance pointer).
while read -r file fn prefix; do
  body=$(awk "/^(CHAIN_INTERNAL )?void $fn\(/,/^\}/" "$file")
  [ -n "$body" ] || fail "could not find $fn in $file"
  printf '%s\n' "$body" | command grep -qE 'chain_mod_clear_target_entries\(inst, [a-z_]+, 0\)' \
    || fail "$fn does not clear the modulation entries of the position it unloads \
without restoring the base, so tests/host/test_chain_reorder_routing.c is testing \
its own fixture"
  # The id must be built for THIS slot, 1-based -- either through
  # chain_fx_component_id (which adds the 1) or by hand. An off-by-one here
  # clears somebody else's modulation and leaves the departing module's behind.
  printf '%s\n' "$body" \
    | command grep -qE "chain_fx_component_id\([a-z_]+, sizeof\([a-z_]+\), \"$prefix\", slot\)|\"$prefix%d\", slot \+ 1" \
    || fail "$fn no longer names its own position as $prefix<slot+1>"
done <<'EOF'
src/modules/chain/dsp/chain_host.c v2_unload_audio_fx_slot fx
src/modules/chain/dsp/chain_midi.c v2_unload_midi_fx_slot midi_fx
EOF

# THE PICKER SWAP, which is the other way a module leaves a position and the
# one the permutation does NOT handle -- a swap writes a single `<id>:module`
# and never reaches chain_reorder.c at all. What stops the arriving module from
# inheriting the departing one's modulation is that the slot loader unloads
# first. The MIDI half of this is proven behaviourally in
# tests/host/test_chain_midi_fx_slot.c ("a failed load clears the addressed
# slot"); the audio half lives in the TU that cannot be compiled natively.
while read -r file fn unloader; do
  body=$(awk "/^(CHAIN_INTERNAL )?(static )?int $fn\(/,/^\}/" "$file")
  [ -n "$body" ] || fail "could not find $fn in $file"
  printf '%s\n' "$body" | command grep -q "$unloader(inst, slot)" \
    || fail "$fn does not unload the slot before loading into it, so a picker swap \
leaves the departing module's modulation entries pointing at the position the NEW \
module now occupies -- it inherits them"
done <<'EOF'
src/modules/chain/dsp/chain_host.c v2_load_audio_fx_slot v2_unload_audio_fx_slot
src/modules/chain/dsp/chain_midi.c v2_load_midi_fx_slot v2_unload_midi_fx_slot
EOF

# --------------------------------------------------------------------------
# THE COMPLETENESS OF THE RETARGET.
#
# Three tables in chain_instance_t name a chain position by string, and all
# three had to be found by READING -- knob_mapping_t calls the field "target"
# just like the other two but lives nowhere near them, and it was nearly
# missed. A fourth added later would be missed in exactly the same way, and the
# symptom is a routing that silently drives whichever module slid into the
# index it names.
#
# So this does not check for the three by name. It derives them: any type with
# a `char target[...]` member is position-keyed, every chain_instance_t array
# of such a type must be walked by chain_reorder.c's retarget.
# --------------------------------------------------------------------------
hdr=src/modules/chain/dsp/chain_internal.h
lfo=src/host/lfo_common.h
src=src/modules/chain/dsp/chain_reorder.c

keyed=$(awk '
  /^typedef struct/ { buf=""; keyed=0 }
  { buf = buf $0 "\n"; if ($0 ~ /char[ \t]+target\[/) keyed=1 }
  /^\} [a-z_0-9]+_t;/ { if (keyed) { name=$2; sub(";","",name); print name } keyed=0 }
' "$hdr" "$lfo" | sort -u)

[ -n "$keyed" ] || fail "could not derive the set of position-keyed types from $hdr"

struct_body=$(awk '/^typedef struct chain_instance \{/,/^\} chain_instance_t;/' "$hdr")

# A derivation that quietly yields nothing passes every check below it, which is
# the exact failure mode this file is guarding against elsewhere. Count what was
# actually examined and require it to be plausible.
checked=0

for type in $keyed; do
  # `|| true`: a type with no DIRECT chain_instance_t member is now a legitimate
  # case (see the nested branch below), and without this the failing grep in a
  # command substitution takes the assignment's exit status down with it -- which
  # under `set -e` ends the script silently, and a silent end reads as a pass.
  members=$(printf '%s\n' "$struct_body" | command grep "^ *$type " \
    | sed -E "s/^ *$type +([a-z_0-9]+)\[.*/\1/" || true)

  if [ -n "$members" ]; then
    for m in $members; do
      checked=$((checked + 1))
      command grep -q "inst->$m\[" "$src" || fail \
"chain_instance_t.$m is an array of $type, which names a chain position by \
string, but chain_reorder.c never walks it. A shape edit will leave every \
routing in it pointing at whatever module slides into the index it names. \
Add it to chain_perm_retarget_all."
    done
    continue
  fi

  # No direct member. A position-keyed type may also be reached one level
  # down -- knob_dest_t lives in an array inside knob_mapping_t, which is the
  # chain_instance_t member. Being nested makes it no less position-keyed:
  # every one of those destinations names a chain position by string too, and
  # a walk that only ever touched index 0 would leave the rest aimed at
  # whatever slid into their index.
  inner=$(command grep -E "^ *$type +[a-z_0-9]+\[" "$hdr" \
    | sed -E "s/^ *$type +([a-z_0-9]+)\[.*/\1/" | sort -u)
  [ -n "$inner" ] || fail "found the position-keyed type $type but no member of it anywhere in $hdr"

  for m in $inner; do
    checked=$((checked + 1))
    # The walk must index it, and must not stop at the first one: a literal
    # [0] and nothing else is exactly the shape that reads as handled.
    command grep -q "\.$m\[" "$src" || command grep -q -- "->$m\[" "$src" || fail \
"$type is nested as .$m[] inside a chain_instance_t member, and names a chain \
position by string, but chain_reorder.c never indexes it. Add it to \
chain_perm_retarget_all."
    # A NON-NUMERIC subscript. `[a-z_0-9]+` would have accepted the literal
    # `[0]` this check exists to reject, which is the whole failure it guards
    # against -- it passed a deliberately-broken walk when first written.
    if [ "$(command grep -c -E "[.>]$m\[[a-z_][a-z_0-9]*\]" "$src" || true)" -eq 0 ]; then
      fail "chain_reorder.c only ever reaches .$m[] at a literal index, so it \
walks one entry and leaves the others pointing at a renumbered position."
    fi
  done
done

[ "$checked" -ge 3 ] || fail "only $checked position-keyed table(s) were derived from $hdr; \
there are three (mod_targets, lfos, knob_mappings), so the derivation above is broken \
and everything it checked was vacuous"

# ...and each of those walks must handle the module that LEFT, not only the one
# that moved: chain_perm_retarget returns -1 for a position that is gone, and a
# walk that ignores the return keeps a routing aimed at a dead module.
# `|| true`: grep -c exits non-zero on zero matches, and under `set -e` that
# would abort the script silently -- which reads as a pass to a CI loop.
n=$(command grep -c 'chain_perm_retarget(' "$src" || true)
[ "$n" -ge "$checked" ] \
  || fail "expected a chain_perm_retarget call per position-keyed table in $src, found $n for $checked tables"

echo "PASS: chain reorder routing — the shipped unloaders match the fixture, and every"
echo "      position-keyed table in chain_instance_t is retargeted by chain_reorder.c"
