#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Drives the REAL Master FX loader out of shadow_chain_mgmt.c on the dev
# machine, against a throwaway audio FX plugin built here, and checks that no
# position ever loses its owned chain_params buffer. See the header comment in
# test_master_fx_cache_ownership.c for why a source pin was not enough.

work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-mfx-cache.XXXXXX")"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/fixture"

cat > "$work/fixture.c" <<'EOF'
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "audio_fx_api_v2.h"

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    return malloc(1);   /* any non-NULL handle */
}
static void fx_destroy(void *instance) { free(instance); }
static void fx_process(void *instance, int16_t *audio_inout, int frames) {
    (void)instance; (void)audio_inout; (void)frames;
}
static void fx_set_param(void *instance, const char *key, const char *val) {
    (void)instance; (void)key; (void)val;
}
static int fx_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    if (key && strcmp(key, "chain_params") == 0 && buf && buf_len > 0) {
        const char *v = "[{\"key\":\"mix\",\"type\":\"float\",\"min\":0,\"max\":1}]";
        int n = (int)strlen(v);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, v, (size_t)n);
        buf[n] = '\0';
        return n;
    }
    return -1;
}

static audio_fx_api_v2_t api = {
    AUDIO_FX_API_VERSION_2,
    fx_create, fx_destroy, fx_process, fx_set_param, fx_get_param, NULL
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &api;
}
EOF

# The loader caches chain_params out of this file, through the owned pointer.
cat > "$work/fixture/module.json" <<'EOF'
{
  "id": "mfx-cache-fixture",
  "component_type": "audio_fx",
  "capabilities": { "chainable": true },
  "chain_params": [
    {"key": "mix", "name": "Mix", "type": "float", "min": 0, "max": 1, "step": 0.01}
  ]
}
EOF

# The handful of externs shadow_chain_mgmt.c reaches for. None of them is on
# any path this test walks; they exist so the unit links.
cat > "$work/stubs.c" <<'EOF'
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

char sampler_current_set_name[128];
char sampler_current_set_uuid[64];

_Atomic int schwung_trace_on = 0;
uint32_t schwung_trace_intern(const char *name) { (void)name; return 0; }
uint64_t schwung_trace_now_ns(void) { return 0; }
void schwung_trace_span_explicit(uint32_t a, uint64_t b, uint64_t c,
                                 uint64_t d, uint64_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}

int set_page_current = 0;
int set_page_read_persisted(void) { return 0; }
void shadow_batch_migrate_sets(void) {}
int shadow_load_config_from_dir(const char *dir) { (void)dir; return 0; }
void shadow_save_state(void) {}
int shadow_chain_midi_inject(const uint8_t *msg, int len) {
    (void)msg; (void)len; return 0;
}
void unified_log(const char *source, int level, const char *fmt, ...) {
    (void)source; (void)level; (void)fmt;
}
EOF

# -lm because shadow_chain_mgmt.c pulls fmod/roundf in through the LFO
# tick. macOS folds libm into libSystem, so a missing -lm links fine there and
# only fails on the Linux CI runner -- which is exactly what it did.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -shared -fPIC \
  -Isrc/host \
  "$work/fixture.c" -o "$work/fixture/dsp.so"

bin="$work/test_master_fx_cache_ownership"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  -DFIXTURE_DSP_PATH="\"$work/fixture/dsp.so\"" \
  tests/host/test_master_fx_cache_ownership.c "$work/stubs.c" \
  -lm -o "$bin"

"$bin"
