#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Runs the REAL MIDI FX loader out of chain_midi.c on the dev machine, against
# two throwaway plugins built here, and checks which slot each load landed in.
# See the header comment in test_chain_midi_fx_slot.c for why a source pin was
# not enough.

work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-midi-fx-slot.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# chain_internal.h includes <malloc.h>, which is glibc-only. One shim header so
# the file compiles on macOS as well as Linux; it changes nothing about the
# code under test.
mkdir -p "$work/shim"
cat > "$work/shim/malloc.h" <<'EOF'
#include <stdlib.h>
EOF

# The loader resolves "<module_dir>/../midi_fx/<name>/dsp.so".
cat > "$work/fixture_fx.c" <<'EOF'
#include <stdint.h>
#include <stdlib.h>
#include "host/midi_fx_api_v1.h"

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    return malloc(1);   /* any non-NULL handle */
}
static void fx_destroy(void *instance) { free(instance); }
static int fx_process(void *instance, const uint8_t *in_msg, int in_len,
                      uint8_t out_msgs[][3], int out_lens[], int max_out) {
    (void)instance; (void)in_msg; (void)in_len;
    (void)out_msgs; (void)out_lens; (void)max_out;
    return 0;
}
static int fx_tick(void *instance, int frames, int sample_rate,
                   uint8_t out_msgs[][3], int out_lens[], int max_out) {
    (void)instance; (void)frames; (void)sample_rate;
    (void)out_msgs; (void)out_lens; (void)max_out;
    return 0;
}
static void fx_set_param(void *instance, const char *key, const char *val) {
    (void)instance; (void)key; (void)val;
}
static int fx_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance; (void)key; (void)buf; (void)buf_len;
    return -1;
}

static midi_fx_api_v1_t api = {
    MIDI_FX_API_VERSION,
    fx_create, fx_destroy, fx_process, fx_tick, fx_set_param, fx_get_param
};

midi_fx_api_v1_t *move_midi_fx_init(const struct host_api_v1 *host) {
    (void)host;
    return &api;
}
EOF

mkdir -p "$work/chain"   # must exist: the loader path goes through "chain/.."

for name in mfxa mfxb; do
  mkdir -p "$work/midi_fx/$name"
  cc -std=gnu11 -shared -fPIC -Isrc "$work/fixture_fx.c" \
     -o "$work/midi_fx/$name/dsp.so"
done

bin="build/tests/test_chain_midi_fx_slot"
mkdir -p "$(dirname "$bin")"

# glibc before 2.34 keeps dlopen in libdl; macOS and current glibc have it in
# libc and reject the flag by warning, not by failing, so probe once.
dl_flag=""
if ! cc -std=gnu11 -x c -o /dev/null - <<'EOF' >/dev/null 2>&1
#include <dlfcn.h>
int main(void) { return dlopen("", 0) != 0; }
EOF
then
  dl_flag="-ldl"
fi

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -I"$work/shim" -Isrc -Isrc/modules/chain/dsp -Isrc/host \
  -DFIXTURE_DIR="\"$work\"" \
  tests/host/test_chain_midi_fx_slot.c src/modules/chain/dsp/chain_midi.c \
  -o "$bin" $dl_flag

"$bin"
