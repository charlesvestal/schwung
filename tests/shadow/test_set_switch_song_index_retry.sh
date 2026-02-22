#!/usr/bin/env bash
set -euo pipefail

shim_c="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

func_body="$(awk '
  /^static void shadow_poll_current_set\(void\)$/ { in_fn=1 }
  in_fn { print }
  in_fn && /^}/ { exit }
' "$shim_c")"

if [ -z "$func_body" ]; then
  echo "FAIL: could not locate shadow_poll_current_set() in $shim_c" >&2
  exit 1
fi

if ! printf '%s\n' "$func_body" | rg -q "shadow_handle_set_loaded\\("; then
  echo "FAIL: shadow_poll_current_set() no longer calls shadow_handle_set_loaded()" >&2
  exit 1
fi

if ! rg -q "sampler_pending_song_index" "$shim_c"; then
  echo "FAIL: unresolved song index tracking is missing" >&2
  exit 1
fi

if ! printf '%s\n' "$func_body" | rg -q "sampler_pending_song_index = -1;"; then
  echo "FAIL: pending song index is not cleared after UUID resolution" >&2
  exit 1
fi

if ! printf '%s\n' "$func_body" | rg -q "sampler_pending_song_index = song_index;"; then
  echo "FAIL: unresolved song index is not marked pending for retry" >&2
  exit 1
fi

if ! printf '%s\n' "$func_body" | rg -q "__pending-"; then
  echo "FAIL: unresolved set selection does not use synthetic pending UUID" >&2
  exit 1
fi

echo "PASS: shadow_poll_current_set retries unresolved song index changes"
exit 0
