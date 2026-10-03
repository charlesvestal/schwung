#!/usr/bin/env bash
# NOT pipefail: every check here is `producer | grep -q ... || say`, and under
# pipefail grep's early exit hands the producer a SIGPIPE that fails the
# pipeline -- timing-dependent, green on macOS and red in CI. Without it a
# pipeline's status is grep's, which is the question each check asks.
set -eu
cd "$(dirname "$0")/../.."

# A CHAIN SLOT'S MODULE LOADS MUST NOT RUN ON THE SPI CALLBACK.
#
# `synth:module`, `fxN:module`, `midi_fxN:module`, `load_file`, `load_patch`
# and `clear` reached the chain host's set_param from
# shadow_inprocess_handle_param_request -- SCHED_FIFO 70, core 3 -- which then
# dlopened, ran create_instance and restored state synchronously. param-slow
# measured `load_file` at 432 ms on hardware: the whole device silent for
# that long, the hiccup heard on every module load. See slot_load_job.h.
#
# What this pins, each of which is a way to put the hiccup back silently:
#
#   1. The param handler routes slot_load_key_is_async keys to
#      shadow_slot_load_post, and RETURNS without answering.
#   2. PARK BEFORE POST: the instance pointer is nulled before the job is
#      published to the loader. The other order lets the callback render an
#      instance the loader is already rebuilding.
#   3. REINSTALL BEFORE ANSWER: the tick puts the pointer back and runs the
#      activation BEFORE it publishes the response, or a client that reads
#      straight after the write sees a parked slot.
#   4. The tick runs every frame, ahead of the request-type check, and the
#      handler serves nothing while a load is in flight.
#   5. The loader is its own thread, started from shim init, demoted to
#      SCHED_OTHER -- not the 200 ms shim worker, and not created from the
#      callback (it would inherit FIFO 70).
#   6. The RT paths that would load/unload ON the callback, or drop a command
#      for the parked slot, wait while a load is in flight.
#   7. The synchronous path and the async completion run ONE activation.

c=src/host/shadow_chain_mgmt.c
s=src/schwung_shim.c
fail=0
say() { echo "FAIL: $1" >&2; fail=1; }

# ---- 1. the handler hands the load off and does not answer ----------------
handler=$(awk '/^void shadow_inprocess_handle_param_request\(void\) \{/,/^}/' "$c")
[ -n "$handler" ] || say "shadow_inprocess_handle_param_request is gone"
printf '%s\n' "$handler" | grep -A2 'slot_load_key_is_async(key_copy)' \
  | grep -q 'shadow_slot_load_post(slot, req_id, key_copy, value_copy)' \
  || say "the SET path no longer hands slot module loads to shadow_slot_load_post"
printf '%s\n' "$handler" | grep -A3 'shadow_slot_load_post(slot, req_id' | grep -q 'return;' \
  || say "a posted load must RETURN unanswered -- the response is the tick's to publish"

# ---- 2. park before post; a SYNTH job is never parked ----------------------
paw=$(awk '/^static void shadow_slot_load_park_and_wake\(/,/^}/' "$c")
[ -n "$paw" ] || say "shadow_slot_load_park_and_wake is gone"
park_at=$(printf '%s\n' "$paw" | grep -n 'instance = NULL' | head -1 | cut -d: -f1 || true)
pub_at=$(printf '%s\n' "$paw" | grep -n 'SLOT_LOAD_WORKING' | head -1 | cut -d: -f1 || true)
wake_at=$(printf '%s\n' "$paw" | grep -n 'shim_slot_loader_wake()' | head -1 | cut -d: -f1 || true)
{ [ -n "$park_at" ] && [ -n "$pub_at" ] && [ -n "$wake_at" ]; } \
  || say "park_and_wake must park, publish and wake"
if [ -n "$park_at" ] && [ -n "$pub_at" ] && [ "$park_at" -gt "$pub_at" ]; then
  say "the slot must be parked BEFORE the job is published to the loader"
fi
post=$(awk '/^static int shadow_slot_load_post\(/,/^}/' "$c")
[ -n "$post" ] || say "shadow_slot_load_post is gone"
synth_blk=$(printf '%s\n' "$post" | awk '/if \(staged\) \{/{on=1} on{print} on&&/return 1;/{exit}')
[ -n "$synth_blk" ] || say "shadow_slot_load_post has no STAGED branch"
printf '%s\n' "$synth_blk" | grep -q 'instance = NULL\|park_and_wake' \
  && say "a STAGED (one-module) job must not park the slot: the rest of it has to keep running"
printf '%s\n' "$post" | grep -q 'if (strcmp(cur, value) == 0) staged = 0;' \
  || say "re-picking the SAME synth must park (unload, then load): one-per-device modules refuse a second instance"
printf '%s\n' "$post" | grep -q 'fade.target = 0.0f' \
  || say "a PARK job must fade the slot out before parking it"

# ---- 3. tick: reinstall / swap, activate, then answer ----------------------
tick=$(awk '/^static int shadow_slot_load_tick\(/,/^}/' "$c")
[ -n "$tick" ] || say "shadow_slot_load_tick is gone"
order() {  # $1 label, then patterns that must appear in this order in $2
  local label=$1 text=$2; shift 2; local last=0 at
  for pat in "$@"; do
    at=$(printf '%s\n' "$text" | grep -n -- "$pat" | head -1 | cut -d: -f1 || true)
    [ -n "$at" ] || { say "$label: missing '$pat'"; return; }
    [ "$at" -gt "$last" ] || { say "$label: '$pat' is out of order"; return; }
    last=$at
  done
}
synth_tick=$(printf '%s\n' "$tick" | awk '/SLOT_LOAD_KIND_STAGED/{on=1} on{print} on&&/return 0;/{exit}')
order "staged swap" "$synth_tick" 'shadow_chain_load_swap_step' 'shadow_slot_after_forwarded_write' 'SLOT_LOAD_RETIRING' 'shadow_slot_load_answer'
park_tick=$(printf '%s\n' "$tick" | awk '/PARK: reinstall/{on=1} on{print}')
order "park" "$park_tick" 'instance = slot_load_job.instance' 'fade.target = slot_load_job.saved_fade_target' 'shadow_slot_after_forwarded_write' 'shadow_slot_load_answer'

# ---- 4. the tick gates the handler, before the request is read -------------
tick_at=$(printf '%s\n' "$handler" | grep -n 'if (shadow_slot_load_tick(shadow_param)) return;' | head -1 | cut -d: -f1 || true)
rt_at=$(printf '%s\n' "$handler" | grep -n 'uint8_t req_type = ' | head -1 | cut -d: -f1 || true)
[ -n "$tick_at" ] || say "the handler no longer runs shadow_slot_load_tick (and stops) every frame"
if [ -n "$tick_at" ] && [ -n "$rt_at" ] && [ "$tick_at" -gt "$rt_at" ]; then
  say "the load tick must run before req_type is read: a load in flight serves nothing"
fi

# ---- 5. its own thread, from init, demoted ---------------------------------
w=src/host/shim_worker.c
loader=$(awk '/^static void \*slot_loader_main\(/,/^}/' "$w")
[ -n "$loader" ] || say "the slot loader thread (shim_worker.c slot_loader_main) is gone"
printf '%s\n' "$loader" | grep -q 'pthread_setschedparam(pthread_self(), SCHED_OTHER' \
  || say "the slot loader must demote itself to SCHED_OTHER"
printf '%s\n' "$loader" | grep -q 'sem_wait(&slot_loader_sem)' \
  || say "the slot loader must wake on the semaphore, not poll"
grep -q '^    shadow_slot_load_start();' "$s" \
  || say "shadow_slot_load_start is not called from shim init"
n=$(grep -c 'shadow_slot_load_start()' "$s" || true)
[ "$n" = "1" ] || say "shadow_slot_load_start should be started exactly once, from init (found $n)"

# ---- 6. paths that wait while a load is in flight --------------------------
for fn in shadow_process_fade_completions shadow_inprocess_handle_ui_request; do
  # Body captured first, THEN searched: `sed ... | grep -q` under pipefail
  # fails whenever grep exits on its first match and sed takes the SIGPIPE --
  # timing-dependent, so it passed on macOS and failed in CI.
  body=$(sed -n "/^void ${fn}(void) {/,/^}/p" "$c")
  case "$body" in
    *'shadow_slot_load_busy()'*) ;;
    *) say "${fn} loads/unloads on the callback and must wait while a slot load is in flight" ;;
  esac
done
grep -q '!shadow_slot_load_busy() &&' "$s" \
  || say "Move-model chain commands must stay queued while a slot is parked"

# ---- 7. one activation -----------------------------------------------------
n=$(grep -c 'shadow_slot_after_forwarded_write(' "$c" || true)
[ "$n" = "4" ] || say "expected one definition and three callers of shadow_slot_after_forwarded_write (found $n)"

# ---- 8. a parked slot passes nothing, not even Move's track dry ------------
grep -q 'else if (have_move_track && shadow_slot_load_parked_slot() != s)' "$s" \
  || say "the mixer must not pass a parked slot's Move track through dry mid-fade"

# ---- 9. a second load waits out a SYNTH job's retire -----------------------
printf '%s\n' "$handler" | grep -B1 'slot_load_key_is_async(shadow_param->key))' | grep -q 'shadow_slot_load_busy()' \
  || say "a load arriving during a retire must be left pending"

[ "$fail" = "0" ] && echo "test_slot_load_off_callback: ok"
exit "$fail"
