#!/usr/bin/env bash
# schwung-manager (Go) pokes shadow_control_t BY BYTE OFFSET, from constants
# written by hand in schwung-manager/shmconfig.go. Nothing tied them to the C
# struct, and one drifted: sampler_source_request and sampler_silent were
# inserted ahead of skipback_seconds, which moved it from 60 to 62 while the
# manager kept writing 60. Setting Skipback length from the web UI then forced
# the sampler source to Resample, could silence the sampler's announcements,
# and never changed the buffer length.
#
# This asks the compiler for offsetof() and sizeof() of every field the Go
# code names, and fails on any disagreement -- including a new `off*`
# constant that is not in the table below, so the next one cannot drift
# silently either.
set -euo pipefail
cd "$(dirname "$0")/../.."
GO=schwung-manager/shmconfig.go
T=$(mktemp -d "${TMPDIR:-/tmp}/mgr_shm.XXXXXX"); trap 'rm -rf "$T"' EXIT

# Go constant -> C field of shadow_control_t
MAP='
offDisplayMode display_mode
offShadowReady shadow_ready
offShouldExit should_exit
offMidiReady midi_ready
offWriteIdx write_idx
offReadIdx read_idx
offUISlot ui_slot
offUIFlags ui_flags
offUIPatchIndex ui_patch_index
offUIFlagsExt ui_flags_ext
offUIRequestID ui_request_id
offShimCounter shim_counter
offSelectedSlot selected_slot
offShiftHeld shift_held
offOvertakeMode overtake_mode
offRestartMove restart_move
offTTSEnabled tts_enabled
offTTSVolume tts_volume
offTTSPitch tts_pitch
offTTSSpeed tts_speed
offOverlayKnobs overlay_knobs_mode
offDisplayMirror display_mirror
offTTSEngine tts_engine
offPinChallenge pin_challenge_active
offDisplayOverlay display_overlay
offOverlayRectX overlay_rect_x
offOverlayRectY overlay_rect_y
offOverlayRectW overlay_rect_w
offOverlayRectH overlay_rect_h
offTTSDebounce tts_debounce_ms
offSetPages set_pages_enabled
offSkipbackReqVol skipback_require_volume
offOpenToolCmd open_tool_cmd
offSkipbackSeconds skipback_seconds
offStayInShadow stay_in_shadow
'

{
  echo '#include <stdio.h>'
  echo '#include <stddef.h>'
  echo '#include "shadow_constants.h"'
  echo '#define O(g, f) printf("%s %zu %zu\n", g, offsetof(shadow_control_t, f), sizeof(((shadow_control_t *)0)->f))'
  echo 'int main(void) {'
  echo "$MAP" | while read -r g f; do if [ -n "$g" ]; then echo "  O(\"$g\", $f);"; fi; done
  echo '  return 0; }'
} > "$T/o.c"
${CC:-cc} -Isrc/host -Isrc -o "$T/o" "$T/o.c"
"$T/o" > "$T/want"

fails=0
# every off* constant in the Go file must be in the table
for g in $(grep -oE '^\s*off[A-Za-z]+\s*=' "$GO" | tr -d ' \t='); do
  if ! echo "$MAP" | grep -qE "^$g "; then
    echo "FAIL $g is in $GO but not in this test's table -- add its C field"; fails=$((fails+1))
  fi
done
while read -r g off size; do
  line=$(grep -E "^\s*$g\s*=" "$GO" | head -1)
  got=$(echo "$line" | sed -E 's/^[^=]*=\s*([0-9]+).*/\1/')
  if [ -z "$line" ]; then echo "FAIL $g missing from $GO"; fails=$((fails+1)); continue; fi
  if [ "$got" != "$off" ]; then
    echo "FAIL $g: Go says $got, shadow_control_t says $off"; fails=$((fails+1)); continue
  fi
  # the width the Go comment declares must be the field's width
  ty=$(echo "$line" | grep -oE '(uint8|uint16|uint32|float32)' | head -1 || true)
  case "$ty" in uint8) w=1;; uint16) w=2;; uint32|float32) w=4;; *) w="";; esac
  if [ -n "$w" ] && [ "$w" != "$size" ]; then
    echo "FAIL $g: Go treats it as $ty, the C field is $size byte(s)"; fails=$((fails+1)); continue
  fi
  echo "ok   $g = $off"
done < "$T/want"
[ "$fails" -eq 0 ] && echo "test_manager_shm_offsets: all passed"
[ "$fails" -eq 0 ]
