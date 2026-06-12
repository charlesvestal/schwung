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

# 8. Sampler/preview heavy I/O must not run in the RT bodies: recording
#    start/stop do mkdir/fopen/pthread_join/WAV-trim; preview does
#    open/mmap. RT arms state and posts to the worker. (The armed-debug PCM
#    dump fopens are the documented exception — arming a debug tap is
#    accepted to glitch audio; their *trigger checks* are worker-published
#    bits, covered by the access() assertion above.)
mix=$(awk '/^static void shadow_inprocess_mix_from_buffer/,/^}/' "$shim")
for bad in 'pthread_create(' 'sampler_stop_recording(' 'preview_play(' \
           'sampler_start_recording_to(' 'sampler_cmd_path.txt' 'PREVIEW_CMD_PATH'; do
  if grep -Fq "$bad" <<<"$mix"; then
    echo "FAIL: shadow_inprocess_mix_from_buffer still uses ${bad} on the RT thread" >&2
    exit 1
  fi
done
nondump_fopen=$(grep -F 'fopen(' <<<"$mix" | grep -cv '_f\[t\]\|_f\[s\]\|align_move_f\|align_synth_f' || true)
if [ "$nondump_fopen" -ne 0 ]; then
  echo "FAIL: mix_from_buffer has non-debug-dump fopen() on the RT thread" >&2
  exit 1
fi
for bad in 'sampler_stop_recording(' 'sampler_start_recording(' 'sampler_start_preroll('; do
  if grep -Fq "$bad" <<<"$post"; then
    echo "FAIL: shim_post_transfer still calls ${bad}) synchronously on the RT thread" >&2
    exit 1
  fi
done

# 9. The per-block capture path must not take a mutex shared with the
#    SCHED_OTHER writer thread (priority inversion) — semaphore signaling.
cap=$(awk '/^static void sampler_capture_audio_common/,/^}/' src/host/shadow_sampler.c)
if grep -q 'pthread_mutex_lock' <<<"$cap"; then
  echo "FAIL: sampler capture still locks a mutex per block on the RT thread" >&2
  exit 1
fi

# 10. Stop is split: RT request + worker finalize (join/trim/close).
if ! rg -q 'void sampler_request_stop\(void\)' src/host/shadow_sampler.c; then
  echo "FAIL: sampler_request_stop (RT half of stop) missing" >&2
  exit 1
fi
if ! rg -q 'sampler_worker_finalize|sampler_finalize_stop' src/host/shadow_sampler.c; then
  echo "FAIL: worker-side sampler finalize missing" >&2
  exit 1
fi

# 11. Skipback save spawn happens on the worker, not the RT gesture path.
sb=$(awk '/^void skipback_trigger_save/,/^}/' src/host/shadow_sampler.c)
if grep -q 'pthread_create' <<<"$sb"; then
  echo "FAIL: skipback_trigger_save still creates a thread on the RT path" >&2
  exit 1
fi

# 12. TTS get_audio runs on the RT mix path: no mutex (priority inversion
#     with the SCHED_OTHER synth thread) and no save_state/file I/O on the
#     disable edge.
for eng in flite espeak; do
  ga=$(awk "/^int ${eng}_tts_get_audio/,/^}/" "src/host/tts_engine_${eng}.c")
  if grep -q 'pthread_mutex_lock' <<<"$ga"; then
    echo "FAIL: ${eng}_tts_get_audio takes a mutex on the RT mix path" >&2
    exit 1
  fi
  if grep -Eq 'save_state\(\)|unified_log' <<<"$ga"; then
    echo "FAIL: ${eng}_tts_get_audio does file I/O / logging on the RT mix path" >&2
    exit 1
  fi
done
if rg -q 'pthread_mutex_t ring_mutex|pthread_mutex_lock\(&ring_mutex' src/host/tts_engine_flite.c; then
  echo "FAIL: flite ring_mutex still present" >&2
  exit 1
fi

echo "PASS: SPI callback path is free of access()/system()/FIFO-inheriting forks"
