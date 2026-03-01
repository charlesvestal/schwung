#!/usr/bin/env bash
set -euo pipefail

shim_c="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# We need a dedicated "pending set" path for selectedSongIndex values that do
# not yet have a materialized Sets/<UUID>/... folder.
if ! rg -q "sampler_pending_song_index" "$shim_c"; then
  echo "FAIL: missing sampler_pending_song_index tracking for unresolved set selection" >&2
  exit 1
fi

# Polling must continue while the same unresolved index remains selected.
if ! (rg -q "song_index == sampler_last_song_index &&" "$shim_c" && \
      rg -q "song_index != sampler_pending_song_index" "$shim_c") && \
   ! rg -q "song_index != sampler_last_song_index && song_index != sampler_pending_song_index" "$shim_c"; then
  echo "FAIL: unresolved song index is not retried while waiting for set materialization" >&2
  exit 1
fi

# Unresolved set selection should map to a synthetic pending UUID so shadow UI
# can switch immediately into a blank state directory.
if ! rg -q "__pending-" "$shim_c"; then
  echo "FAIL: no synthetic pending UUID namespace for pre-creation set state" >&2
  exit 1
fi

if ! rg -q "shadow_handle_set_loaded\\(pending_name, pending_uuid\\)" "$shim_c"; then
  echo "FAIL: unresolved set selection does not trigger pending set load" >&2
  exit 1
fi

# Once the real UUID resolves, pending state must be cleared.
if ! rg -q "sampler_pending_song_index = -1;" "$shim_c"; then
  echo "FAIL: pending set tracking is not cleared after UUID resolution" >&2
  exit 1
fi

echo "PASS: pending selected-set path enters blank state before UUID folder exists"
exit 0
