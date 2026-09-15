#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves:
#
#   RUN   lane_tick() out of the real chain_lanes.c, driving the real
#         chain_mod.c and lane_store.c against a fake synth that records what
#         each key received (tests/host/test_chain_lanes_playback.c).
#
#   PIN   that lane_tick is called from BOTH tick paths. chain_host.c dlopens
#         plugins and owns the get_param/set_param surface, so it cannot be
#         compiled natively; the wiring is checked at the source level, the
#         same way test_chain_knob_cc_out.sh checks its three emit sites.

fail() { echo "FAIL: $1"; exit 1; }

# ------------------------------------------------------------------ run half
bin="build/tests/test_chain_lanes_playback"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, which is glibc-only; one shim header
# lets this compile on macOS too and changes nothing about the code under test.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_lanes_playback.c \
  src/modules/chain/dsp/chain_lanes.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  src/host/lane_store.c \
  src/host/lane_serial.c \
  -o "$bin"

"$bin"

# ------------------------------------------------------------------ pin half

# BOTH TICK PATHS, OR THE LANE FREEZES IN SILENCE. The shim skips render_block
# on a silent slot (one probe frame in 172) and drives "mod:tick" instead, so a
# lane_tick only inside render_block stops advancing the moment the slot goes
# quiet and then jumps when a note comes back -- the same defect that made LFOs
# run ~172x too slow on an idle slot.
#
# Checked as "every lfo_tick call is followed by lane_tick", rather than as a
# count, so the failure names the site that lost it.
host=src/modules/chain/dsp/chain_host.c
missing=$(awk '
    /lfo_tick\(inst, frames\);/ { pending = NR; next }
    pending && /^[[:space:]]*$/ { next }
    pending {
        if ($0 !~ /lane_tick\(inst\);/) print pending
        pending = 0
    }
    END { if (pending) print pending }
' "$host")
[ -z "$missing" ] || fail "lfo_tick at $host:$(echo "$missing" | tr '\n' ' ')has no lane_tick beside it -- a lane freezes on whichever path that is"

n=$(command grep -c 'lane_tick(inst);' "$host" || true)
[ "$n" -eq 2 ] || fail "$host has $n lane_tick call sites, expected exactly 2 (render_block and mod:tick); a third would double-tick a frame"

# The store lives on the INSTANCE. patch_info_t is a stack local on the SPI
# callback (v2_set_param's load_file) AND sits MAX_PATCHES deep inside the
# instance, so a lane_store_t in there lands in both multipliers -- raising
# SLOT_BUSES 4 -> 8 once took that callback frame from 194 KB to 232 KB.
command grep -q 'lane_store_t lanes;' src/modules/chain/dsp/chain_internal.h \
    || fail "the lane store is no longer a named member of chain_instance_t"
# Accumulate from the LAST `typedef struct {` before the closing line, rather
# than using an awk range -- a range starting at the first anonymous typedef
# would swallow every struct in between and report them as patch_info_t's.
patch_body=$(awk '/^typedef struct \{/ { buf = "" }
                  { buf = buf $0 "\n" }
                  /^\} patch_info_t;/ { printf "%s", buf; exit }' \
             src/modules/chain/dsp/chain_internal.h)
if printf '%s' "$patch_body" | command grep -q 'lane_store_t'; then
    fail "a lane_store_t is inside patch_info_t -- that struct is a stack local on the SPI callback"
fi

echo "PASS: lanes play against clip phase on both tick paths; unknown phase releases"
