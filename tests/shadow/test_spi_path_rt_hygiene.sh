#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

shim="src/schwung_shim.c"

# RT pass 1 (docs/plans/2026-06-11-codebase-cleanup-review.md §1):
# the SPI callbacks (shim_pre_transfer, shim_post_transfer,
# shadow_inprocess_mix_from_buffer) run at SCHED_FIFO 90 with a ~900µs
# budget. Flag-file access() polling, trigger-file consumption, and hook
# execution belong on the shim worker thread, which publishes bits the
# RT path reads.

# 1. The worker exists and polls debug flags off the RT thread.
if [ ! -f src/host/shim_worker.c ]; then
  echo "FAIL: src/host/shim_worker.c missing" >&2
  exit 1
fi
if ! rg -q 'shim_debug_flags' src/host/shim_worker.c; then
  echo "FAIL: shim_worker.c does not publish shim_debug_flags" >&2
  exit 1
fi
if ! rg -q 'SCHED_OTHER' src/host/shim_worker.c; then
  echo "FAIL: shim worker thread does not pin itself to SCHED_OTHER" >&2
  exit 1
fi

# 2. No access() syscalls remain in the SPI callback bodies.
for fn in 'static void shim_pre_transfer' 'static void shim_post_transfer' \
          'static void shadow_inprocess_mix_from_buffer'; do
  body=$(awk "/^${fn}/,/^}/" "$shim")
  if [ -z "$body" ]; then
    echo "FAIL: could not extract ${fn}" >&2
    exit 1
  fi
  n=$(grep -c 'access(' <<<"$body" || true)
  if [ "$n" -ne 0 ]; then
    echo "FAIL: ${fn} still calls access() ${n}x on the RT thread" >&2
    exit 1
  fi
done

# 3. No raw system() in shim_post_transfer — overtake exit hooks must be
#    deferred to the worker.
post=$(awk '/^static void shim_post_transfer/,/^}/' "$shim")
if grep -q 'system(' <<<"$post"; then
  echo "FAIL: shim_post_transfer still calls system() on the RT thread" >&2
  exit 1
fi
if ! grep -q 'shim_worker_post(' <<<"$post"; then
  echo "FAIL: overtake exit hook is not deferred via shim_worker_post" >&2
  exit 1
fi

# 4. shim_run_command children must drop SCHED_FIFO before exec.
runcmd=$(awk '/^static int shim_run_command/,/^}/' "$shim")
if ! grep -q 'SCHED_OTHER' <<<"$runcmd"; then
  echo "FAIL: shim_run_command child inherits SCHED_FIFO 90" >&2
  exit 1
fi

# 5. The set poll's filesystem walk runs on the worker; the SPI path only
#    consumes the published snapshot.
pre=$(awk '/^static void shim_pre_transfer/,/^}/' "$shim")
if grep -q 'shadow_poll_current_set()' <<<"$pre"; then
  echo "FAIL: shadow_poll_current_set (opendir/getxattr walk) still on SPI thread" >&2
  exit 1
fi
if ! grep -q 'shadow_set_pages_consume()' <<<"$pre"; then
  echo "FAIL: SPI path does not consume the set-page snapshot" >&2
  exit 1
fi

# 6. shadow_midi_out_log_enabled must not access() on every call.
mol=$(awk '/^int shadow_midi_out_log_enabled/,/^}/' src/host/shadow_chain_mgmt.c)
if [ -z "$mol" ]; then
  mol=$(awk '/shadow_midi_out_log_enabled\(void\)/,/^}/' src/host/shadow_chain_mgmt.c)
fi
if ! grep -Eq 'check_counter|% *200|% *100' <<<"$mol"; then
  echo "FAIL: shadow_midi_out_log_enabled calls access() on every call" >&2
  exit 1
fi

# 7. chain_host parse_debug_log must not stat() on every v2_set_param.
pdl=$(awk '/^static void parse_debug_log\(/,/^}/' src/modules/chain/dsp/chain_host.c)
if ! grep -Eq 'counter|cached' <<<"$pdl"; then
  echo "FAIL: chain_host parse_debug_log stats the flag file on every set_param" >&2
  exit 1
fi

echo "PASS: SPI callback path is free of access()/system()/FIFO-inheriting forks"
