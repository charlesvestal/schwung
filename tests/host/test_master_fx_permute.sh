#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves.
#
#   RUN   drive the real shim code -- the real loader, real dlopen, real owned
#         buffers -- through load / insert / move / remove / unload, and assert
#         invariants, ordering and LFO routing separately. See the header of
#         tests/host/test_master_fx_permute.c.
#
#   PIN   the completeness of the collector, which the run half cannot reach:
#         a per-position array that is not registered keeps the value belonging
#         to whatever module USED to be at that index, and for the param caches
#         that means position 3 serving position 5's metadata.
#
# Why the pin half is not test_chain_permute.sh's: that one derives the array
# list from a STRUCT DEFINITION (every [MAX_AUDIO_FX] member of
# chain_instance_t). Three of Master FX's per-position arrays are file-static,
# outside any struct, so a struct-shaped derivation is structurally blind to
# exactly the ones most likely to be forgotten. Derive from the FILE instead.

fail() { echo "FAIL: $1" >&2; exit 1; }

# Every derivation below ends in `|| true`. grep exits non-zero on zero matches,
# and under `set -o pipefail` that aborts the whole script at the assignment --
# exit 1 with NO diagnostic, which tells a reader nothing about which guard
# tripped. The explicit emptiness checks after each derivation are the ones that
# should speak; this is what lets them run at all.

hdr=src/host/shadow_chain_mgmt.h
src=src/host/shadow_chain_mgmt.c

# --------------------------------------------------------------------------
# THE ENUMERATION. Every [MASTER_FX_SLOTS] array -- declared in either file --
# must appear in mfx_perm_collect.
#
# `shadow_master_fx_slots` appears twice (extern in the header, definition in
# the .c); sort -u collapses that.
# --------------------------------------------------------------------------
# Unindented lines only: a declaration at file scope starts in column 0, while
# the verbs' local scratch arrays (`char *owned[MASTER_FX_SLOTS];`) are inside
# a function body and are not per-position STATE. The floor below is what
# catches this filter if it ever stops matching.
arrays=$(command grep -hE '^[^[:space:]].*\[MASTER_FX_SLOTS\]' "$hdr" "$src" \
  | command grep -oE '[a-z_0-9]+\[MASTER_FX_SLOTS\]' \
  | sed -E 's/\[MASTER_FX_SLOTS\]//' | sort -u || true)

[ -n "$arrays" ] || fail "could not derive any [MASTER_FX_SLOTS] array from $hdr / $src"

collector=$(awk '/^static int mfx_perm_collect\(/,/^\}/' "$src")
[ -n "$collector" ] || fail "could not find mfx_perm_collect in $src"

# A derivation that quietly yields nothing passes every check below it, which
# is the failure mode this file exists to prevent. Count what was examined.
checked=0
for a in $arrays; do
  checked=$((checked + 1))
  printf '%s\n' "$collector" | command grep -qE "MFX_PERM_(FIELD|OWNED)\($a\)" \
    || fail "$a is a per-position Master FX array but mfx_perm_collect never \
registers it. A shape edit will leave its value belonging to whatever module \
USED to be at that index."
done

# 4 today: shadow_master_fx_slots plus the three file-static caches. A new one
# raises this; a derivation that stops finding them lowers it, and that is what
# this floor catches.
[ "$checked" -ge 4 ] || fail "only $checked [MASTER_FX_SLOTS] array(s) were derived \
from $hdr / $src; there are at least four, so the derivation above is broken and \
everything it checked was vacuous"

# ...and the reverse: the collector must not claim an array that is gone or was
# never per-position.
for a in $(printf '%s\n' "$collector" \
           | command grep -oE 'MFX_PERM_(FIELD|OWNED)\([a-z_0-9]+\)' \
           | sed -E 's/.*\(([a-z_0-9]+)\)/\1/' | sort -u || true); do
  # owned_ptrs is the scratch array the collector lifts the struct's owned
  # pointers into for the duration of the permutation; it is per-position by
  # construction and has no [MASTER_FX_SLOTS] declaration of its own.
  [ "$a" = "owned_ptrs" ] && continue
  printf '%s\n' "$arrays" | command grep -qx "$a" \
    || fail "mfx_perm_collect permutes '$a', which is not a [MASTER_FX_SLOTS] array"
done

# --------------------------------------------------------------------------
# THE CLASSIFICATION, which is the one that reached hardware.
#
# A per-position array is either a VALUE array (vacating a position zeroes its
# bytes) or an OWNED-BUFFER array -- a pointer to a block allocated once per
# position and NEVER NULL. Owned arrays must be ROTATED: nulling the pointer
# for a vacated position leaves a NULL that the next module load writes a param
# table through, which is a SIGSEGV on the SCHED_FIFO SPI callback, and the
# shift also leaks the allocation it overwrites.
#
# The two kinds are indistinguishable BY SHAPE -- a char* array and a value
# array holding pointers look identical -- so the enumeration above cannot tell
# them apart, and the hand-written classification must not be trusted. The set
# of owned buffers is not a matter of opinion: it is exactly what
# shadow_master_fx_storage_ensure() allocates. Derive it from there and compare.
# (This is the same trap that let the slot chain's permutation ship with 28/28
# mutations killed and segfault anyway.)
# --------------------------------------------------------------------------
ensure=$(awk '/^int shadow_master_fx_storage_ensure\(/,/^\}/' "$src")
[ -n "$ensure" ] || fail "could not find shadow_master_fx_storage_ensure in $src"

# Both spellings of an allocation target: the file-static array, and the member
# reached through the struct array. Flattened first -- the assignments wrap
# across lines, and a line-oriented match would find neither and then pass
# every check below it.
owned=$(printf '%s\n' "$ensure" | tr '\n' ' ' \
  | command grep -oE '(shadow_master_fx_slots\[i\]\.[a-z_0-9]+|[a-z_0-9]+\[i\]) *= *calloc' \
  | sed -E 's/ *=.*//; s/shadow_master_fx_slots\[i\]\./STRUCT:/; s/\[i\]//' \
  | sort -u || true)

[ -n "$owned" ] || fail "could not read the owned-buffer set out of \
shadow_master_fx_storage_ensure"

owned_checked=0
for o in $owned; do
  owned_checked=$((owned_checked + 1))
  case "$o" in
    STRUCT:*)
      # An owned pointer INSIDE the struct array cannot be registered as an
      # owned array directly -- chain_permute.h classifies a whole array, and
      # the struct must travel as values (memset on vacate would null the
      # pointer: the SIGSEGV). So it must be lifted out and put back.
      member=${o#STRUCT:}
      printf '%s\n' "$collector" \
        | command grep -qE "owned_ptrs\[i\] *= *shadow_master_fx_slots\[i\]\.$member" \
        || fail "shadow_master_fx_slots[].$member is allocated per position by \
shadow_master_fx_storage_ensure but mfx_perm_collect does not lift it out into an \
owned array. The struct travels as VALUES, so vacating a position would memset \
that pointer to NULL -- a null deref inside the next module load, on the audio \
thread."
      restore=$(awk '/^static void mfx_perm_restore_owned\(/,/^\}/' "$src")
      printf '%s\n' "$restore" \
        | command grep -qE "shadow_master_fx_slots\[i\]\.$member *= *owned_ptrs\[i\]" \
        || fail "mfx_perm_collect lifts $member out but mfx_perm_restore_owned never \
puts it back -- every position would be left with a NULL owned buffer"
      ;;
    *)
      printf '%s\n' "$collector" | command grep -q "MFX_PERM_OWNED($o)" \
        || fail "$o is allocated per position by shadow_master_fx_storage_ensure but \
is NOT registered as MFX_PERM_OWNED in mfx_perm_collect. Vacating a position would \
zero its POINTER instead of its contents -- a null deref inside the next module \
load, on the audio thread."
      ;;
  esac
done

[ "$owned_checked" -ge 2 ] || fail "only $owned_checked owned buffer(s) were derived \
from shadow_master_fx_storage_ensure; there are two (the struct member and the \
file-static runtime cache), so the derivation is broken and its checks were vacuous"

# ...and nothing else may claim to be one: rotating a value array would treat
# its bytes as an address.
for a in $(printf '%s\n' "$collector" | command grep -oE 'MFX_PERM_OWNED\([a-z_0-9]+\)' \
           | sed -E 's/.*\(([a-z_0-9]+)\)/\1/' | sort -u || true); do
  [ "$a" = "owned_ptrs" ] && continue
  printf '%s\n' "$owned" | command grep -qx "$a" \
    || fail "mfx_perm_collect registers '$a' as an owned buffer, but \
shadow_master_fx_storage_ensure never allocates one for it"
done

# --------------------------------------------------------------------------
# VACATING GOES THROUGH THE UNLOADER. The struct holds a dlopen handle and an
# FX instance; a memset leaks both, silently -- quieter than the slot chain's
# crash and therefore worse.
# --------------------------------------------------------------------------
body=$(awk '/^int shadow_master_fx_remove\(/,/^\}/' "$src")
[ -n "$body" ] || fail "could not find shadow_master_fx_remove in $src"
printf '%s\n' "$body" | command grep -q 'shadow_master_fx_slot_unload(at)' \
  || fail "shadow_master_fx_remove does not vacate through \
shadow_master_fx_slot_unload; anything else leaks the dlopen handle and the FX \
instance"

# --------------------------------------------------------------------------
# THE LFO BASE. mfx_lfo_base_valid[] is indexed BY LFO, not by position, so
# re-aiming lfo->target is not enough: a remove that clears a target must also
# invalidate that LFO's base, or it modulates its next target around a stale
# one. The run half asserts the behaviour; this pins that the clear lives in
# the retarget, where a future author will look.
# --------------------------------------------------------------------------
body=$(awk '/^static void mfx_perm_retarget_lfos\(/,/^\}/' "$src")
[ -n "$body" ] || fail "could not find mfx_perm_retarget_lfos in $src"
printf '%s\n' "$body" | command grep -q 'mfx_lfo_base_valid\[i\] = 0' \
  || fail "mfx_perm_retarget_lfos does not invalidate the LFO base when it clears a \
departed target. mfx_lfo_base_valid[] is indexed by LFO, so the next target the user \
picks would be modulated around the removed module's value."

# --------------------------------------------------------------------------
# NO CACHED POINTER TO POSITION 0's CAPTURE RULES.
#
# The run half proves the union is right today. These two pin the shape that
# made it wrong: a `#define` naming position 0's capture rules, and a raw
# pointer to them cached once at init in shadow_midi.c. A pointer captured at
# init into an array whose contents PERMUTE is a bug whichever position it
# names, and reintroducing either would restore "a MIDI-triggered module goes
# silent when someone reorders the chain" without failing the run half, which
# calls the union directly.
# --------------------------------------------------------------------------
if command grep -qE '^#define[[:space:]]+shadow_master_fx_capture' "$hdr"; then
  fail "shadow_chain_mgmt.h defines shadow_master_fx_capture again. It names \
position 0's rules, and the last thing that used it cached a pointer to it at init \
-- so a capturing module was heard only while it sat first."
fi
if command grep -qE 'static[[:space:]]+shadow_capture_rules_t[[:space:]]*\*' src/host/shadow_midi.c; then
  fail "shadow_midi.c caches a shadow_capture_rules_t pointer again. Master FX \
capture must be read per event from the live array: a pointer taken at init keeps \
aiming at one index while insert/remove/move rotate the modules underneath it."
fi

# ---------------------------------------------------------------- run half
work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-mfx-permute.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cat > "$work/fixture.c" <<'EOF'
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "audio_fx_api_v2.h"

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    return malloc(1);   /* any non-NULL handle; distinct per instance */
}
static void fx_destroy(void *instance) { free(instance); }
static void fx_process(void *instance, int16_t *audio_inout, int frames) {
    (void)instance; (void)audio_inout; (void)frames;
}
static void fx_set_param(void *instance, const char *key, const char *val) {
    (void)instance; (void)key; (void)val;
}
static int fx_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance; (void)key; (void)buf; (void)buf_len;
    return -1;
}

static audio_fx_api_v2_t api = {
    AUDIO_FX_API_VERSION_2,
    fx_create, fx_destroy, fx_process, fx_set_param, fx_get_param, NULL
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &api;
}
EOF

cat > "$work/stubs.c" <<'EOF'
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

char sampler_current_set_name[128];
char sampler_current_set_uuid[64];

_Atomic int schwung_trace_on = 0;
uint32_t schwung_trace_intern(const char *name) { (void)name; return 0; }
uint64_t schwung_trace_now_ns(void) { return 0; }
void schwung_trace_span_explicit(uint32_t a, uint64_t b, uint64_t c,
                                 uint64_t d, uint64_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}

int set_page_current = 0;
int set_page_read_persisted(void) { return 0; }
void shadow_batch_migrate_sets(void) {}
int shadow_load_config_from_dir(const char *dir) { (void)dir; return 0; }
void shadow_save_state(void) {}
int shadow_chain_midi_inject(const uint8_t *msg, int len) {
    (void)msg; (void)len; return 0;
}
void unified_log(const char *source, int level, const char *fmt, ...) {
    (void)source; (void)level; (void)fmt;
}
EOF

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -shared -fPIC \
  -Isrc/host "$work/fixture.c" -o "$work/fixture.so"

# One module directory per position: module_id is derived from the parent
# directory name, so a module's identity is observable in a failure message
# rather than being just a pointer. Distinct paths also mean distinct dlopen
# handles, which is what makes "no handle lost" checkable.
slots=$(awk '/^#define MASTER_FX_SLOTS /{print $3}' src/host/shadow_chain_mgmt.h)
[ -n "$slots" ] || fail "could not read MASTER_FX_SLOTS from src/host/shadow_chain_mgmt.h"
i=0
while [ "$i" -lt "$slots" ]; do
  mkdir -p "$work/mfx$i"
  cp "$work/fixture.so" "$work/mfx$i/dsp.so"
  printf '{"id":"mfx%d","component_type":"audio_fx","capabilities":{"chainable":true}}\n' \
    "$i" > "$work/mfx$i/module.json"
  i=$((i + 1))
done

# One more fixture, the only one that DECLARES capture. The run half loads it
# at each position in turn: a module that asked for MIDI must be heard wherever
# it sits, or moving a ducker off position 0 takes it off the air.
mkdir -p "$work/mfxcap"
cp "$work/fixture.so" "$work/mfxcap/dsp.so"
printf '%s\n' '{"id":"mfxcap","component_type":"audio_fx","capabilities":{"chainable":true,"capture":{"groups":["pads","tracks"]}}}' \
  > "$work/mfxcap/module.json"

bin="$work/test_master_fx_permute"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Isrc/host \
  -DFIXTURE_DIR="\"$work\"" \
  tests/host/test_master_fx_permute.c "$work/stubs.c" \
  -o "$bin"

"$bin"

echo "PASS: Master FX permute enumeration — every [MASTER_FX_SLOTS] array in the"
echo "      header and the .c is registered, and the owned/value split is derived"
echo "      from shadow_master_fx_storage_ensure rather than trusted"
