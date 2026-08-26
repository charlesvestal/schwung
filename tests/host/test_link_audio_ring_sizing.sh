#!/usr/bin/env bash
#
# The Move→shim Link Audio ring must be sized for MOVE'S MEASURED JITTER, and
# the catch-up threshold must sit above the burst it is meant to absorb.
#
# Why this test exists
# --------------------
# Both numbers were wrong at once on 2026-08-27, and each made the other
# invisible:
#
#   - the ring was 16 blocks (46 ms) because it was aliased to the PUB ring's
#     size "for symmetry". Move stalls ~92 ms — twice the whole ring.
#   - catch-up fired above 3072 samples (35 ms), a value calibrated when
#     "observed bursts were consistently <30 ms". Bursts are now ~85 ms, so
#     every refill that could have covered the next stall was discarded:
#     ~76,000 stereo samples per 5 s window, ~17% of the audio.
#
# Result was ~16% of frames with no Move audio at all — audible as repeated
# gaps, and diagnosed for a whole session as everything except a buffer size.
#
# So this pins the ENGINEERING MARGINS against the measured hardware numbers,
# not the constants themselves. Changing 64 to 128 passes. Changing it back to
# 16 fails, and says why.
#
# The measured numbers are recorded in src/host/link_audio.h.

set -u
cd "$(dirname "$0")/../.."

fail=0
ok()   { echo "  ok:   $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

HDR=src/host/link_audio.h
SRC=src/host/shadow_link_audio.c

# --- measured hardware behaviour these margins must cover -------------------
MEASURED_STALL_MS=92     # cb max_gap, all four channels together
MEASURED_BURST_MS=85     # cb max_burst_run = 30 blocks of 125 frames
RATE=44100

# --- resolve the macros by asking the compiler, not by regex ----------------
# A regex over #defines gets the arithmetic wrong the moment one of them is
# expressed in terms of another, which all of these are.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/probe.c" <<'EOF'
#include <stdio.h>
#include "link_audio.h"
int main(void) {
    printf("%d %d %d %d\n",
           LINK_AUDIO_IN_RING_SAMPLES,
           LINK_AUDIO_IN_RING_MASK,
           LINK_AUDIO_IN_CATCHUP_SAMPLES,
           LINK_AUDIO_IN_BLOCK_SAMPLES);
    return 0;
}
EOF

if ! cc -I src/host -o "$TMP/probe" "$TMP/probe.c" 2>"$TMP/err"; then
    echo "  FAIL: could not compile the macro probe:"
    sed 's/^/        /' "$TMP/err"
    exit 1
fi

read -r RING MASK CATCHUP BLOCK < <("$TMP/probe")

ring_ms=$(( RING * 1000 / 2 / RATE ))
catchup_ms=$(( CATCHUP * 1000 / 2 / RATE ))

echo "link audio IN ring sizing"
echo "  ring    = $RING stereo samples (${ring_ms} ms)"
echo "  catchup = $CATCHUP stereo samples (${catchup_ms} ms)"
echo

# --- 1. power of two, or the mask silently corrupts the wrap ----------------
if [ $(( RING & MASK )) -eq 0 ] && [ $(( RING )) -gt 0 ]; then
    ok "ring is a power of two (the mask is used as & instead of %)"
else
    bad "ring $RING is NOT a power of two — LINK_AUDIO_IN_RING_MASK is broken"
fi

# --- 2. the ring must outlast the stall, with margin ------------------------
# Bare equality is not enough: the ring has to hold the stall AND the burst
# that follows it, or the producer laps the consumer on the way back up.
if [ "$ring_ms" -ge $(( MEASURED_STALL_MS * 2 )) ]; then
    ok "ring ${ring_ms}ms covers 2x the measured ${MEASURED_STALL_MS}ms stall"
else
    bad "ring ${ring_ms}ms is under 2x the measured ${MEASURED_STALL_MS}ms stall — a stall will empty it"
fi

# --- 3. catch-up must NOT discard the burst --------------------------------
if [ "$catchup_ms" -gt "$MEASURED_BURST_MS" ]; then
    ok "catch-up at ${catchup_ms}ms is above the measured ${MEASURED_BURST_MS}ms burst"
else
    bad "catch-up at ${catchup_ms}ms would DISCARD the measured ${MEASURED_BURST_MS}ms burst — starve/burst/discard loop"
fi

# --- 4. ...but must still fire before the producer laps us ------------------
if [ "$CATCHUP" -lt "$RING" ]; then
    ok "catch-up sits inside the ring, so a runaway producer is still caught"
else
    bad "catch-up ($CATCHUP) >= ring ($RING) — it can never fire"
fi

# --- 5. the IN ring must not be aliased to the PUB ring again ---------------
# This is how it got the wrong size in the first place: the two sit next to
# each other in the header and one was defined in terms of the other. They
# have different producers with different timing and must be sized apart.
if grep -qE '^#define[[:space:]]+LINK_AUDIO_IN_RING_BLOCKS[[:space:]]+LINK_AUDIO_PUB' "$HDR"; then
    bad "LINK_AUDIO_IN_RING_BLOCKS is aliased to the PUB ring again — that IS the bug"
else
    ok "IN ring has its own size, not the PUB ring's"
fi

# --- 6. a layout change must bump the SHM version ---------------------------
# The struct grew from 32 KB to 128 KB. A v2 segment left by an older sidecar
# is SHORTER than the shim's mapping, and touching the tail of an undersized
# mapping is SIGBUS. The version check is the only thing that rejects it, and
# it only works if the number actually moved.
VER=$(grep -E '^#define[[:space:]]+LINK_AUDIO_IN_SHM_VERSION' "$HDR" | awk '{print $3}')
if [ "${VER:-0}" -ge 3 ]; then
    ok "SHM version is $VER (>= 3, bumped for the ring resize)"
else
    bad "SHM version is ${VER:-unset} — the ring grew, so an old sidecar's segment is too SHORT for our mapping (SIGBUS). Bump it."
fi

# magic/version must remain the first two fields, or the version check itself
# reads off the end of an undersized mapping.
if grep -A3 'typedef struct {' "$HDR" | grep -B1 'volatile uint32_t version;' | grep -q 'volatile uint32_t magic;'; then
    ok "magic/version are still the first fields (safe to read on a short mapping)"
else
    bad "magic/version are no longer first — a short-segment check would SIGBUS before it could reject"
fi

# --- 7. the threshold must be DERIVED, not a literal at the call site -------
# The old value was a bare `need * 12` inside link_audio_read_channel_shm, so
# resizing the ring left it behind. Nothing in the reader may re-introduce a
# hand-written multiple of `need`.
if grep -qE 'avail > need \* [0-9]+' "$SRC"; then
    bad "reader compares avail against a literal multiple of need again — resize the ring and this is stale"
else
    ok "reader uses the derived LINK_AUDIO_IN_CATCHUP_SAMPLES"
fi

if grep -q 'LINK_AUDIO_IN_CATCHUP_SAMPLES' "$SRC"; then
    ok "reader references the derived threshold"
else
    bad "reader does not use LINK_AUDIO_IN_CATCHUP_SAMPLES"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: link audio ring is sized for the measured jitter"
else
    echo "FAIL: link audio ring sizing"
fi
exit "$fail"
