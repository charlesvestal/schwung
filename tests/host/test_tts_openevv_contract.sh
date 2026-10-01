#!/usr/bin/env bash
set -euo pipefail

# openevv (Eloquence) as the third screen reader engine: the properties that
# fail SILENTLY rather than loudly.
#
#   1. The ECI numbers tts_engine_openevv.c declares for itself (so the shim
#      compiles without the submodule) must be eci.h's. A wrong sample-rate
#      code or voice-param index is not an error -- it is a voice that sounds
#      subtly wrong, or 11 kHz audio played at 44.1.
#   2. schwung-manager reads the seven voice bytes at a RAW offset. Computed
#      here from the C struct, never restated, as test_stay_in_shadow.sh does.
#   3. The preset table exists THREE times (the device menu, the web manager,
#      and openevv's own enus.settings). A preset that loads the wrong six
#      values is a voice that is not the one chosen, with nothing logged.
#   4. tts.json has ONE writer. Three writers each rewrote the whole file from
#      their own key list, so a key one of them did not know reverted on its
#      next save -- the install.sh / features.json defect, in miniature.
#   5. The shim never LINKS libeci: dlopened, a missing library falls back to
#      eSpeak; linked, it is a MoveOriginal that does not start.
#   6. No ECI call from the SPI path: every eci.* call belongs to the worker.

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
fail() { echo "FAIL: $*" >&2; FAILED=1; }
FAILED=0

SRC=src/host/tts_engine_openevv.c
ECI_H=libs/openevv/include/eci.h
SETTINGS=libs/openevv/lang/enus/enus.settings

if [ ! -f "$ECI_H" ] || [ ! -f "$SETTINGS" ]; then
    echo "FAIL: libs/openevv is not checked out ($ECI_H)." >&2
    echo "      git submodule update --init libs/openevv" >&2
    exit 1
fi

# --- 1. ECI numbers ----------------------------------------------------------
probe="${TMPDIR:-/tmp}/eci_probe_$$"
cat > "$probe.c" <<'CEOF'
#include <stdio.h>
#include "eci.h"
#define P(n) printf(#n "=%d\n", (int)(n))
int main(void) {
    P(eciWaveformBuffer); P(eciDataNotProcessed); P(eciDataProcessed); P(eciDataAbort);
    P(eciSampleRate);
    P(eciGender); P(eciHeadSize); P(eciPitchBaseline); P(eciPitchFluctuation);
    P(eciRoughness); P(eciBreathiness); P(eciSpeed); P(eciVolume);
    return 0;
}
CEOF
eci_vals=$(cc -I"$ROOT/libs/openevv/include" -o "$probe" "$probe.c" 2>&1 && "$probe") ||
    { fail "could not compile the eci.h probe: $eci_vals"; eci_vals=""; }
rm -f "$probe" "$probe.c"
while IFS='=' read -r name val; do
    [ -n "$name" ] || continue
    grep -Eq "\\b${name} = ${val}\\b" "$SRC" ||
        fail "$SRC does not declare $name = $val as eci.h does"
done <<< "$eci_vals"
# The sample-rate CODE: 1 is 11025 in eci.h's table (8000, 11025, 22050, ...).
# The backend asks for the engine's native rate and upsamples 4x itself
# (tts_upsample4.h); any other rate played through that 4x is speech at the
# wrong speed -- chipmunks or a drawl, not an error.
grep -q '#define OPENEVV_SAMPLE_RATE_11025 1' "$SRC" ||
    fail "the eciSampleRate code for 11025 Hz must be 1"
grep -q 'eciSampleRate, OPENEVV_SAMPLE_RATE_11025' "$SRC" ||
    fail "the engine must be asked for 11025: its own 44.1 kHz sinc cost 11x the speech"
grep -q 'tts_up4_run' "$SRC" ||
    fail "11025 Hz audio must go through tts_upsample4.h before the 44.1 kHz ring"

# The worker WAITS with eciSynchronize. The engine hands over about one buffer
# per eciSpeaking call, so a sleep-and-poll on it paces the whole delivery,
# first sample included, at the poll interval (1055 ms vs 262 for a sentence).
grep -q 'eci\.Speaking' "$SRC" &&
    fail "the openevv worker polls eciSpeaking -- wait with eciSynchronize"
grep -q 'eci.Synchronize(h)' "$SRC" ||
    fail "the openevv worker must wait for an utterance with eciSynchronize"
# ...and a full ring is waited out IN the callback: eciDataNotProcessed makes
# the engine sleep a flat 30 ms, and an interruption landing there waits it out.
awk '/^static int openevv_callback/,/^}/' "$SRC" | grep -q 'return eciDataNotProcessed' &&
    fail "the openevv callback answers eciDataNotProcessed -- a 30 ms engine sleep; wait for room instead"

# --- 2. the manager's raw offset --------------------------------------------
probe="${TMPDIR:-/tmp}/evv_off_$$"
cat > "$probe.c" <<'CEOF'
#include <stdio.h>
#include <stddef.h>
#include "src/host/shadow_constants.h"
int main(void) {
    printf("%zu %zu\n", offsetof(shadow_control_t, tts_evv_voice),
                        offsetof(shadow_control_t, tts_evv_breath));
    return 0;
}
CEOF
offs=$( (cc -I. -o "$probe" "$probe.c" 2>/dev/null) && "$probe" ) || offs=""
rm -f "$probe" "$probe.c"
if [ -z "$offs" ]; then
    fail "could not compile the offsetof probe -- does shadow_control_t still carry tts_evv_*?"
else
    c_voice=${offs% *}; c_breath=${offs#* }
    [ $((c_breath - c_voice)) -eq 6 ] ||
        fail "the seven tts_evv_* bytes must be contiguous in voice..breath order (the manager indexes them)"
    go_off=$(grep -oE 'offTTSEvvVoice += *[0-9]+' schwung-manager/shmconfig.go | grep -oE '[0-9]+$' || true)
    [ "$go_off" = "$c_voice" ] ||
        fail "schwung-manager reads tts_evv_voice at ${go_off:-none}, the C struct puts it at $c_voice"
fi
grep -q 'TTSEvvFields = \[\]string{"voice", "gender", "head", "pitch", "inflection", "rough", "breath"}' \
    schwung-manager/shmconfig.go ||
    fail "TTSEvvFields must name the bytes in the struct order"

# --- 3. the preset table, three times ---------------------------------------
# enus.settings: VoiceN=gender head pitch fluctuation rough breath speed volume
want=$(grep -E '^Voice[1-8]=' "$SETTINGS" | head -8 | sed 's/^Voice[1-8]=//' |
       awk '{print $1","$2","$3","$4","$5","$6}')
[ "$(printf '%s\n' "$want" | wc -l)" -eq 8 ] || fail "enus.settings no longer declares Voice1..Voice8"

command -v node >/dev/null 2>&1 || { echo "FAIL: node required" >&2; exit 1; }
js=$(node --input-type=module -e '
const G = await import(process.cwd() + "/src/shadow/shadow_ui_global_grid.mjs");
for (const p of G.EVV_PRESETS) console.log([p.gender, p.head, p.pitch, p.inflection, p.rough, p.breath].join(","));
')
[ "$js" = "$want" ] || fail "EVV_PRESETS (shadow_ui_global_grid.mjs) disagrees with enus.settings:
$js
-- want --
$want"

go=$(awk '/^var ttsEvvPresets/,/^}/' schwung-manager/main.go | grep -oE '\{[0-9, ]+\}' |
     tr -d '{} ' )
[ "$go" = "$want" ] || fail "ttsEvvPresets (schwung-manager/main.go) disagrees with enus.settings:
$go
-- want --
$want"

# --- 4. one writer of tts.json -----------------------------------------------
writers=$(grep -lE 'tts\.json' src/host/*.c src/*.c 2>/dev/null | grep -v 'src/host/tts_config.c' || true)
for w in $writers; do
    if grep -E 'fopen\([^)]*"w"' "$w" | grep -q 'tts' ||
       grep -qE 'config_path = "/data/UserData/schwung/config/tts.json"' "$w"; then
        fail "$w opens tts.json itself -- go through tts_config_save (tts_config.h)"
    fi
done
for eng in espeak flite; do
    body=$(awk "/^static void ${eng}_save_config/,/^}/" "src/host/tts_engine_${eng}.c")
    grep -q 'tts_config_save' <<< "$body" ||
        fail "${eng}_save_config does not save through tts_config_save"
done

# --- 5. dlopened, never linked -----------------------------------------------
grep -E '^\s*SHIM_LIBS=' scripts/build.sh | grep -q -- '-leci' &&
    fail "build.sh links the shim against libeci -- it must be dlopened"
grep -q 'dlopen(OPENEVV_LIB' "$SRC" || fail "$SRC no longer dlopens libeci"
grep -q 'src/host/tts_engine_openevv.c' scripts/build.sh ||
    fail "build.sh does not compile tts_engine_openevv.c into the shim"
grep -q 'SCHWUNG_ALLOW_NO_OPENEVV' scripts/build.sh ||
    fail "build.sh must fail on a missing openevv submodule unless SCHWUNG_ALLOW_NO_OPENEVV=1"

# --- 6. no ECI call on the SPI path ------------------------------------------
# Every openevv_tts_* entry point can be reached from shim_pre_transfer.
for fn in $(grep -oE '^(bool|void|int|float) +openevv_tts_[a-z_]+' "$SRC" | awk '{print $2}'); do
    body=$(awk "/^(bool|void|int|float) +${fn}\\(/,/^}/" "$SRC")
    if grep -qE '\beci\.[A-Z]' <<< "$body"; then
        fail "$fn calls into ECI -- only the worker may"
    fi
done
ga=$(awk '/^int openevv_tts_get_audio/,/^}/' "$SRC")
grep -qE 'pthread_mutex|unified_log|fopen|sem_wait' <<< "$ga" &&
    fail "openevv_tts_get_audio blocks, logs or does I/O on the RT mix path"
grep -q 'PTHREAD_EXPLICIT_SCHED' "$SRC" ||
    fail "the openevv worker must be created SCHED_OTHER explicitly, not inherit FIFO 70"
grep -q 'pthread_join' "$SRC" &&
    fail "openevv must never join its worker: cleanup is reached from the SPI path"

# --- 7. engine 2 means openevv everywhere ------------------------------------
grep -q 'engine == 2 ? "openevv"' src/schwung_shim.c ||
    fail "the shim does not map tts_engine 2 to openevv"
grep -q 'shadow_control->tts_engine = 2;' src/shadow/shadow_ui.c ||
    fail "tts_set_engine(\"openevv\") does not write 2"
grep -q 'app.shm.SetTTSEngine(2)' schwung-manager/main.go ||
    fail "the manager cannot select openevv"

[ "$FAILED" -eq 0 ] || exit 1
echo "PASS: openevv -- ECI numbers match eci.h, manager offset $c_voice computed, presets agree three ways, one tts.json writer, dlopened, no ECI on the SPI path"
