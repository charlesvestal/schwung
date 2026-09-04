#!/usr/bin/env bash
# Pin the Go /schwung-perf offset mirror against the C header it mirrors.
#
# schwung-manager/perf_shm.go reads schwung_perf_snapshot_t
# (src/host/perf_snapshot.h) out of a raw /dev/shm segment by HAND-MIRRORED
# byte offsets — there is no shared schema, just a comment above the const
# block asking the next editor to keep the two in sync. shmparams.go carries
# the same kind of mirror, and it already went wrong once: two uint64 trace
# fields were added to shadow_param_t on the C side and the Go mirror was
# never updated, so every key after them landed 16 bytes late, the shim read
# an empty key for every request, and the remote UI quietly showed "no
# module" and default params for every param page. Nothing crashed and no
# test failed — a hand-mirrored layout drifts silently and totally, and only
# a probe that asks the COMPILER where the fields actually live can catch it.
#
# perf_shm_test.go already has TestPerfOffsetsMatchTheHeader, which pins the
# Go ARITHMETIC against hardcoded numbers. That catches a mistake in the Go
# formulas but never reads the C header, so it keeps passing green while a
# field added to the C struct moves every real offset out from under it. This
# test is the other half: it compiles a small C probe against the real
# header, asks Go to print its own constants, and diffs the two by field
# name.
set -uo pipefail
cd "$(dirname "$0")/../.."

fails=0
fail() { echo "FAIL: $1"; fails=$((fails+1)); }

TMPDIR=$(mktemp -d)
DUMP_GO="schwung-manager/zz_perf_offsets_dump_test.go"
cleanup() {
    rm -rf "$TMPDIR"
    rm -f "$DUMP_GO"
}
trap cleanup EXIT

CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "SKIP test_perf_shm_offsets (no C compiler: $CC)"
    exit 0
fi
if ! command -v go >/dev/null 2>&1; then
    echo "SKIP test_perf_shm_offsets (no go toolchain)"
    exit 0
fi

# --- C side: ask the compiler where every field really lives -------------

# name/field pairs: C struct field on the left, the Go constant we expect to
# mirror it on the right. Kept as one table so drift is a one-line edit.
read -r -d '' FIELD_PAIRS <<'PAIRS' || true
magic perfOffMagic
version perfOffVersion
seq perfOffSeq
frame_ready perfOffFrameReady
granular_ready perfOffGranularReady
sample_window_frames perfOffSampleWindow
frame_period_us perfOffFramePeriodUs
frame_total_avg perfOffFrameTotalAvg
frame_total_max perfOffFrameTotalMax
frame_pre_avg perfOffFramePreAvg
frame_pre_max perfOffFramePreMax
frame_ioctl_avg perfOffFrameIoctlAvg
frame_ioctl_max perfOffFrameIoctlMax
frame_post_avg perfOffFramePostAvg
frame_post_max perfOffFramePostMax
midi_mon_avg perfOffSections
post_midi_scan_avg perfOffPostChunks
slot_render_avg perfOffSlotRenderAvg
slot_render_max perfOffSlotRenderMax
slot_synth_avg perfOffSlotSynthAvg
slot_synth_max perfOffSlotSynthMax
slot_fx_avg perfOffSlotFxAvg
slot_fx_max perfOffSlotFxMax
mfx_avg perfOffMfxAvg
mfx_max perfOffMfxMax
overtake_gen_avg perfOffOvertakeGenAvg
overtake_gen_max perfOffOvertakeGenMax
overtake_fx_avg perfOffOvertakeFxAvg
overtake_fx_max perfOffOvertakeFxMax
slot_probe_burst_max perfOffProbeBurstMax
jack_audio_hits perfOffJackAudioHits
jack_audio_misses perfOffJackAudioMisses
overrun_count perfOffOverrunCount
PAIRS

PROBE_C="$TMPDIR/probe.c"
{
    echo '#include <stdio.h>'
    echo '#include <stddef.h>'
    echo '#include "perf_snapshot.h"'
    echo 'int main(void) {'
    echo '    printf("sizeof %zu\n", sizeof(schwung_perf_snapshot_t));'
    while IFS=' ' read -r cfield _; do
        [ -z "$cfield" ] && continue
        printf '    printf("%s %%zu\\n", offsetof(schwung_perf_snapshot_t, %s));\n' \
            "$cfield" "$cfield"
    done <<<"$FIELD_PAIRS"
    echo '    return 0;'
    echo '}'
} > "$PROBE_C"

if ! "$CC" -I src/host -o "$TMPDIR/probe" "$PROBE_C" 2>"$TMPDIR/cc.err"; then
    cat "$TMPDIR/cc.err" >&2
    fail "C probe failed to compile against src/host/perf_snapshot.h"
    echo "$fails check(s) failed"
    exit 1
fi

"$TMPDIR/probe" > "$TMPDIR/c_offsets.txt"

C_SIZEOF=$(awk '$1=="sizeof"{print $2}' "$TMPDIR/c_offsets.txt")
if [ -z "$C_SIZEOF" ]; then
    fail "C probe printed no sizeof line"
elif [ "$C_SIZEOF" -gt 4096 ]; then
    fail "sizeof(schwung_perf_snapshot_t) is $C_SIZEOF, exceeds the fixed 4096-byte mapping the Go reader maps"
fi

# --- Go side: ask Go what it thinks each constant is ----------------------

GO_CONSTS=$(awk '{print $2}' <<<"$FIELD_PAIRS" | sort -u)

{
    echo 'package main'
    echo
    echo 'import ("fmt"; "testing")'
    echo
    echo 'func TestZZDumpPerfOffsets(t *testing.T) {'
    while read -r gname; do
        [ -z "$gname" ] && continue
        printf '\tfmt.Printf("%s %%d\\n", %s)\n' "$gname" "$gname"
    done <<<"$GO_CONSTS"
    echo '}'
} > "$DUMP_GO"

if ! ( cd schwung-manager && go test -run TestZZDumpPerfOffsets -v . ) \
        > "$TMPDIR/go_out.txt" 2>"$TMPDIR/go_err.txt"; then
    cat "$TMPDIR/go_err.txt" >&2
    cat "$TMPDIR/go_out.txt" >&2
    fail "go test failed to build/run the offset dump against schwung-manager/perf_shm.go"
    echo "$fails check(s) failed"
    exit 1
fi

# --- Compare, field by field ----------------------------------------------

while IFS=' ' read -r cfield gname; do
    [ -z "$cfield" ] && continue
    c_off=$(awk -v f="$cfield" '$1==f{print $2}' "$TMPDIR/c_offsets.txt")
    g_off=$(awk -v f="$gname" '$1==f{print $2}' "$TMPDIR/go_out.txt")
    if [ -z "$c_off" ]; then
        fail "C probe printed no offset for field $cfield"
        continue
    fi
    if [ -z "$g_off" ]; then
        fail "go test printed no value for constant $gname (dump program did not run it?)"
        continue
    fi
    if [ "$c_off" != "$g_off" ]; then
        fail "offset drift: C offsetof($cfield) = $c_off, Go $gname = $g_off"
    fi
done <<<"$FIELD_PAIRS"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "PASS test_perf_shm_offsets"
