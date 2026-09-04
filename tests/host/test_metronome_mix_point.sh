#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# WHERE the metronome is mixed is the entire design, and getting it wrong is
# silent in exactly the direction that matters: the click keeps working, and
# quietly appears in every resample and Skipback capture.
#
# The design doc asked for a test asserting the click is present in
# mailbox_audio and absent from unity_view in the same frame. That needs the
# shim built and running, and the shim cannot be built on the dev machine — so
# it is split in two: this STATIC pin on the source order, and the on-device
# resample check. Neither alone shows it; this half is the one CI can run.
#
# The three landmarks, in the order they must appear in shim_post_transfer:
#
#   1. native_capture_total_mix_snapshot_from_buffer(unity_view)
#        unity_view is taken. Everything the sampler, Skipback and the native
#        resample bridge will ever see is fixed at this point.
#   2. shadow_metronome_render(mailbox_audio, ...)
#        the click goes into the DAC mailbox only.
#   3. rebuild_from_la && mv < 0.9999f
#        master volume is applied, so the click tracks the knob.
#
# Move 2 above 1 and the click lands in recordings. Move it below 3 and it
# stops tracking master volume.

SRC=src/schwung_shim.c
failures=0

read -r snap mix vol <<<"$(awk '
    /native_capture_total_mix_snapshot_from_buffer\(unity_view\)/ { snap = NR }
    /shadow_metronome_render\(mailbox_audio/                      { mix  = NR }
    /rebuild_from_la && mv < 0\.9999f/                            { vol  = NR }
    END { print snap+0, mix+0, vol+0 }' "$SRC")"

for pair in "snap:$snap:the unity_view capture snapshot" \
            "mix:$mix:the shadow_metronome_render call" \
            "vol:$vol:the master-volume scaling"; do
    IFS=: read -r _ line what <<<"$pair"
    if [ "$line" -eq 0 ]; then
        echo "FAIL: could not find $what in $SRC — the landmark moved or was renamed," \
             "and this test cannot pin an order it cannot locate" >&2
        failures=$((failures + 1))
    fi
done

if [ "$failures" -eq 0 ]; then
    if [ "$snap" -ge "$mix" ]; then
        echo "FAIL: the metronome is mixed at line $mix, at or before the unity_view" \
             "snapshot at line $snap — the click would be recorded into every" \
             "resample and Skipback capture" >&2
        failures=$((failures + 1))
    fi
    if [ "$mix" -ge "$vol" ]; then
        echo "FAIL: the metronome is mixed at line $mix, at or after the master-volume" \
             "scaling at line $vol — the click would ignore the volume knob" >&2
        failures=$((failures + 1))
    fi
fi

# The render must be gated on rebuild_from_la, or it doubles with Move's own
# metronome everywhere else.
if ! grep -q 'if (rebuild_from_la && shadow_control) {' "$SRC"; then
    echo "FAIL: the metronome render is not gated on rebuild_from_la — outside" \
         "Move->Schwung, Move's own metronome is audible and this doubles it" >&2
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    echo "test_metronome_mix_point: FAIL ($failures)" >&2
    exit 1
fi

echo "PASS: metronome mix point — unity_view snapshot ($snap) < render ($mix) <" \
     "master volume ($vol), gated on rebuild_from_la"
