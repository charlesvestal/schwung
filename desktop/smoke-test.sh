#!/usr/bin/env bash
#
# Load every installed module in a real chain and report whether it makes
# sound.
#
#   desktop/smoke-test.sh [module-root]
#
# "It compiled" and "it works" are different claims, and the gap between them
# is where this port will actually fail: a module can dlopen cleanly, register
# its parameters, and render pure silence because it wanted a sample pack, a
# ROM, or a rate this host does not give it. Finding that out module by module
# in Live is the slow way.
#
# Each synth gets a held chord; each audio FX gets braids in front of it,
# because an effect with no input scores zero however well it works.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
MODULES="${1:-$ROOT/build/desktop/modules}"
RENDER="$ROOT/build/desktop/schwung-render"
TMP="${TMPDIR:-/tmp}/schwung-smoke.$$"

[ -x "$RENDER" ] || { echo "build it first: desktop/build.sh" >&2; exit 1; }
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# A module that renders below this is not making audible sound. Deliberately
# low: the fleet's healthy level is around -20 dBFS rms, and a quiet-but-real
# patch should not be reported as broken.
FLOOR=-60

run_one() {
    local label="$1"; shift
    # --play IS NOT OPTIONAL HERE. The chain answers get_clock_status from
    # MIDI realtime bytes it has actually received, so a harness that never
    # starts a clock reports every tempo-synced module as silent -- which
    # reads as "the module is broken" rather than "the test never pressed
    # play". breakbeat, sequencers, arps and every synced effect are in that
    # class; the first sweep called them all failures.
    local out; out=$("$RENDER" --modules "$MODULES" "$@" --play --bpm 120 --seconds 2 -o "$TMP/o.wav" 2>&1)
    local rms; rms=$(printf '%s\n' "$out" | sed -n 's/.*rms \(-*[0-9.]*\) dBFS.*/\1/p' | head -1)

    if [ -z "$rms" ]; then
        printf '  %-18s FAILED TO LOAD\n' "$label"
        printf '%s\n' "$out" | grep -iE "dlopen|failed|error" | head -1 | sed 's/^/      /'
        return 1
    fi
    if awk -v r="$rms" -v f="$FLOOR" 'BEGIN{exit !(r<f)}'; then
        printf '  %-18s silent (%s dBFS)\n' "$label" "$rms"
        return 1
    fi
    printf '  %-18s ok     (%s dBFS)\n' "$label" "$rms"
    return 0
}

ok=0; bad=0

echo "SOUND GENERATORS"
for d in "$MODULES"/sound_generators/*/; do
    [ -d "$d" ] || continue
    id=$(basename "$d")
    if run_one "$id" --synth "$id"; then ok=$((ok+1)); else bad=$((bad+1)); fi
done

echo
echo "AUDIO FX  (braids in front)"
for d in "$MODULES"/audio_fx/*/; do
    [ -d "$d" ] || continue
    id=$(basename "$d")
    if run_one "$id" --synth braids --fx "$id"; then ok=$((ok+1)); else bad=$((bad+1)); fi
done

echo
echo "working: $ok    not working: $bad"
