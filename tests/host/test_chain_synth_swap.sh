#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Runs the REAL staged module swaps out of chain_synth_load.c and
# chain_fx_load.c against throwaway synths and audio FX built here. See tests/host/test_chain_synth_swap.c for what
# each check guards; the short version is that a slot's synth swap now happens
# in three steps on two threads, and only behaviour can show the order.

work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-synth-swap.XXXXXX")"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/shim"
cat > "$work/shim/malloc.h" <<'H'
#include <stdlib.h>
H

cat > "$work/fixture_synth.c" <<'C'
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "host/plugin_api_v1.h"

int fixture_created;
int fixture_destroyed;

static void *s_create(const char *dir, const char *cfg) { (void)dir; (void)cfg; fixture_created++; return malloc(1); }
static void s_destroy(void *i) { fixture_destroyed++; free(i); }
static void s_on_midi(void *i, const uint8_t *m, int l, int src) { (void)i; (void)m; (void)l; (void)src; }
static void s_set_param(void *i, const char *k, const char *v) { (void)i; (void)k; (void)v; }
static int s_get_param(void *i, const char *k, char *b, int n) { (void)i; (void)k; (void)b; (void)n; return -1; }
static void s_render(void *i, int16_t *out, int frames) { (void)i; memset(out, 0, (size_t)frames * 4); }

static plugin_api_v2_t api;
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    api.api_version = MOVE_PLUGIN_API_VERSION_2;
    api.create_instance = s_create;
    api.destroy_instance = s_destroy;
    api.on_midi = s_on_midi;
    api.set_param = s_set_param;
    api.get_param = s_get_param;
    api.render_block = s_render;
    return &api;
}
C

# An audio FX that writes a constant WET level (or passes through, and counts
# its process calls, for the neighbour that must keep running).
cat > "$work/fixture_fx.c" <<'C'
#include <stdint.h>
#include <stdlib.h>
#include "host/audio_fx_api_v2.h"

int fixture_created;
int fixture_destroyed;
int fixture_processed;

static void *f_create(const char *dir, const char *cfg) { (void)dir; (void)cfg; fixture_created++; return malloc(1); }
static void f_destroy(void *i) { fixture_destroyed++; free(i); }
static void f_process(void *i, int16_t *io, int frames) {
    (void)i; fixture_processed++;
#ifdef WET
    for (int k = 0; k < frames * 2; k++) io[k] = WET;
#else
    (void)io; (void)frames;
#endif
}
static void f_set_param(void *i, const char *k, const char *v) { (void)i; (void)k; (void)v; }
static int f_get_param(void *i, const char *k, char *b, int n) { (void)i; (void)k; (void)b; (void)n; return -1; }

static audio_fx_api_v2_t api;
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    api.api_version = AUDIO_FX_API_VERSION_2;
    api.create_instance = f_create;
    api.destroy_instance = f_destroy;
    api.process_block = f_process;
    api.set_param = f_set_param;
    api.get_param = f_get_param;
    return &api;
}
C

mkdir -p "$work/chain"   # module_dir; the loader resolves "<module_dir>/../<kind>/<id>"
for name in syna synb; do
  mkdir -p "$work/sound_generators/$name"
  cc -std=gnu11 -shared -fPIC -Isrc "$work/fixture_synth.c" -o "$work/sound_generators/$name/dsp.so"
done
build_fx() {  # name, extra flags — audio FX resolve as audio_fx/<id>/<id>.so
  mkdir -p "$work/audio_fx/$1"
  cc -std=gnu11 -shared -fPIC -Isrc $2 "$work/fixture_fx.c" -o "$work/audio_fx/$1/$1.so"
}
build_fx fxa -DWET=1000
build_fx fxb -DWET=2000
build_fx fxc ""

dl_flag=""
if ! cc -std=gnu11 -x c -o /dev/null - <<'C' >/dev/null 2>&1
#include <dlfcn.h>
int main(void) { return dlopen("", 0) != 0; }
C
then
  dl_flag="-ldl"
fi

bin="build/tests/test_chain_synth_swap"
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -I"$work/shim" -Isrc -Isrc/modules/chain/dsp -Isrc/host \
  -DFIXTURE_DIR="\"$work\"" \
  tests/host/test_chain_synth_swap.c src/modules/chain/dsp/chain_synth_load.c \
  src/modules/chain/dsp/chain_fx_load.c \
  -o "$bin" $dl_flag -lm

"$bin"
