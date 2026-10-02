#!/usr/bin/env bash
set -euo pipefail

# tts.json has ONE writer, src/host/tts_config.c.
#
# Three writers (the dispatcher, eSpeak, Flite) each rewrote the whole file
# from their own key list, after parsing the other keys back out by hand. A
# key one of them did not know reverted on its next save -- the install.sh /
# features.json defect, in miniature -- and nothing fails when that happens:
# the file is valid, it is only missing a value.
#
# And that writer takes NO LOCK. The savers run on the SPI path (FIFO 70); a
# mutex shared with a writer on an ordinary thread is a priority inversion
# with no inheritance. The behaviour is in tests/host/test_tts_config.c; this
# pins the shape that a later edit could quietly undo.

cd "$(dirname "$0")/../.."

fail() { echo "FAIL: $*" >&2; exit 1; }

# Nobody else opens the file.
writers=$(grep -lE 'tts\.json' src/host/*.c src/*.c 2>/dev/null | grep -v 'src/host/tts_config.c' || true)
for w in $writers; do
    if grep -E 'fopen\([^)]*"w"' "$w" | grep -q 'tts' ||
       grep -qE 'config_path = "/data/UserData/schwung/config/tts.json"' "$w"; then
        fail "$w opens tts.json itself -- go through tts_config_update (tts_config.h)"
    fi
done

# The engine savers name the fields they own. load + edit + save from two
# threads interleaves and drops the other thread's change.
for eng in espeak flite; do
    body=$(awk "/^static void ${eng}_save_config/,/^}/" "src/host/tts_engine_${eng}.c")
    [ -n "$body" ] || fail "${eng}_save_config not found in src/host/tts_engine_${eng}.c"
    grep -qE 'tts_config_(save|update)' <<< "$body" ||
        fail "${eng}_save_config does not save through tts_config (tts_config.h)"
    if grep -q 'tts_config_load' <<< "$body"; then
        fail "${eng}_save_config loads before saving -- use tts_config_update with its own fields"
    fi
done
body=$(awk '/^static void save_engine_choice/,/^}/' src/host/tts_engine_dispatch.c)
grep -q 'tts_config_update' <<< "$body" ||
    fail "save_engine_choice does not save through tts_config_update (tts_config.h)"

# No lock, and it must not come back.
if grep -qE 'pthread_mutex|pthread\.h|sem_wait|flock\(' src/host/tts_config.c; then
    fail "src/host/tts_config.c takes a lock -- it is reached from the SPI path"
fi
grep -q 'atomic_' src/host/tts_config.c ||
    fail "src/host/tts_config.c no longer merges through atomics"

# The shim is built with it.
grep -E '^\s*SHIM_TTS_SRC=' scripts/build.sh | grep -q 'src/host/tts_config.c' ||
    fail "build.sh does not compile tts_config.c into the screen reader shim"

echo "PASS: tts.json -- one writer, field-masked updates, no lock"
