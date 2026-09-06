#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# MASTER FX AND SEND FX POSITIONS MUST NOT LOAD ON THE SPI CALLBACK, and the
# gate that lets them load off it must be closed before the request is
# published.
#
# fx_slot_load_impl dlopens a shared object, runs create_instance and reads
# module.json. Both `module` param writes reached it from
# shadow_inprocess_handle_param_request, which the shim calls from
# shim_pre_transfer -- SCHED_FIFO 70, core 3, ~2370us for the whole device.
# Loading a 7.7 MB CLAP bundle there was measured on hardware at ~708 dropped
# SPI frames in one write, and every param round trip on the device timed out
# at its 100 ms deadline while it ran.
#
# Bus FX had already been moved to a worker for exactly this reason
# (chain_bus.c); the sends were left on the callback only because Master FX
# was. This pins the fix so it cannot be undone by a merge:
#
#   1. The two `module` SET handlers call shadow_fx_load_request, never a
#      synchronous shadow_*_fx_slot_load*.
#   2. THE CLOSE COMES FIRST. req_seq is bumped before req_path is written and
#      before req_pub publishes it -- the same ordering
#      tests/host/test_bus_gate_ordering.sh pins for buses, and for the same
#      reason: a worker that reads the payload of a request that has already
#      been superseded acts on a torn string.
#   3. Only ONE site bumps req_seq. A second close is a second gate nobody
#      would find.
#   4. The worker does the expensive half, and it is the SHIM WORKER -- which
#      is created off the callback and demotes itself -- not a pthread_create
#      from a module entry point, which would inherit SCHED_FIFO 70 and starve
#      Move's own Link Main at 35.
#   5. The install runs from the per-frame RT entry point, because it is the
#      only writer of the live position structs. That is what lets every
#      render/MIDI/param reader of them stay unguarded.

c=src/host/shadow_chain_mgmt.c
w=src/host/shim_worker.c
fail=0
say() { echo "FAIL: $1" >&2; fail=1; }

command -v rg >/dev/null || { echo "SKIP: rg not available" >&2; exit 0; }

# ---- 1. the two module SET handlers ---------------------------------------
# The send handler and the master handler each write exactly one module key.
for fn in shadow_fx_load_request; do
  n=$(rg -c "int (result|r) = ${fn}\(" "$c" || true)
  [ "${n:-0}" -ge 1 ] || say "no module SET handler calls ${fn}"
done
n=$(rg -c 'int r = shadow_send_fx_slot_load_with_config\(' "$c" || true)
[ "${n:-0}" = "0" ] || say "the send module SET still calls the synchronous loader on the SPI callback"
n=$(rg -c 'int result = shadow_master_fx_slot_load\(' "$c" || true)
[ "${n:-0}" = "0" ] || say "the master module SET still calls the synchronous loader on the SPI callback"

# ---- 2. close, then write, then publish ------------------------------------
body=$(awk '/^int shadow_fx_load_request\(/,/^}/' "$c")
[ -n "$body" ] || say "shadow_fx_load_request is gone"
close_at=$(printf '%s\n' "$body" | grep -n 'req_seq, fx_load_next_seq' | head -1 | cut -d: -f1 || true)
path_at=$(printf '%s\n' "$body" | grep -n 'r->req_path, sizeof' | head -1 | cut -d: -f1 || true)
pub_at=$(printf '%s\n' "$body" | grep -n 'req_pub, r->req_seq' | head -1 | cut -d: -f1 || true)
if [ -z "$close_at" ] || [ -z "$path_at" ] || [ -z "$pub_at" ]; then
  say "shadow_fx_load_request no longer closes, writes the path and publishes"
else
  [ "$close_at" -lt "$path_at" ] || say "the request path is written BEFORE the gate is closed"
  [ "$path_at" -lt "$pub_at" ] || say "the request is published BEFORE its path is written"
fi

# ---- 3. one closer ---------------------------------------------------------
bumps=$(rg -c '__atomic_store_n\(&r->req_seq, fx_load_next_seq' "$c" || true)
[ "${bumps:-0}" -le 2 ] || say "req_seq is bumped at ${bumps} sites; the close belongs in the request and the cancel only"

# ---- 4. the expensive half is the worker's ---------------------------------
worker=$(awk '/^void shadow_fx_load_worker_tick\(/,/^}/' "$c")
printf '%s\n' "$worker" | grep -q 'fx_slot_load_impl(' ||
  say "the worker tick no longer performs the load"
rt=$(awk '/^int shadow_fx_load_request\(/,/^}/' "$c")
for bad in 'dlopen(' 'dlclose(' 'fopen(' 'malloc(' 'calloc(' 'pthread_create('; do
  if printf '%s\n' "$rt" | grep -q -- "$bad"; then
    say "shadow_fx_load_request calls ${bad} on the SPI callback"
  fi
done
inst=$(awk '/^void shadow_fx_load_install_tick\(/,/^}/' "$c")
for bad in 'dlopen(' 'dlclose(' 'fopen(' 'malloc(' 'calloc(' 'destroy_instance(' 'pthread_create(' 'unified_log(' 'fprintf('; do
  if printf '%s\n' "$inst" | grep -q -- "$bad"; then
    say "shadow_fx_load_install_tick calls ${bad} on the SPI callback"
  fi
done
rg -q 'shadow_fx_load_worker_tick\(\);' "$w" ||
  say "the shim worker does not run shadow_fx_load_worker_tick -- nothing loads at all"
if rg -q 'pthread_create\(' "$c"; then
  say "shadow_chain_mgmt.c creates a thread; a pthread_create from a module entry point inherits SCHED_FIFO 70"
fi

# ---- 5. the install is on the RT thread, first ------------------------------
entry=$(awk '/^void shadow_inprocess_handle_param_request\(/,/^}/' "$c")
head=$(printf '%s\n' "$entry" | sed -n '1,14p')
if ! printf '%s\n' "$head" | grep -q 'shadow_fx_load_install_tick();'; then
  say "the install does not run first from the per-frame RT entry point"
fi

[ "$fail" = 0 ] || exit 1
echo "PASS: FX position loading is off the SPI callback and the gate closes before it publishes"
