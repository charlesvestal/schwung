/*
 * Stubs for shadow_rec_source.c dependencies when compiled into shadow_ui binary.
 * The shadow_ui binary doesn't have shadow_chain_mgmt, so we provide minimal
 * implementations of shadow_log and shadow_host_api here.
 */

#include <stdio.h>
#include <stdint.h>
#include "../host/plugin_api_v1.h"

/* shadow_log: just print to stderr */
void shadow_log(const char *msg) {
    fprintf(stderr, "[shadow_ui] %s\n", msg);
}

/* Minimal host API for rec source plugins */
static void stub_log(const char *msg) {
    fprintf(stderr, "[rec_source_plugin] %s\n", msg);
}

/* sampler_source: referenced by shadow_ui.c rec source load/unload.
 * In the shadow_ui process this is local state — the actual sampler runs in the shim
 * and reads its own sampler_source. The shim polls shadow_rec_source.active to know
 * which source to use. */
#include "../host/shadow_sampler.h"
sampler_source_t sampler_source = SAMPLER_SOURCE_RESAMPLE;

host_api_v1_t shadow_host_api = {
    .api_version = 2,
    .sample_rate = 44100,
    .frames_per_block = 128,
    .mapped_memory = NULL,
    .audio_out_offset = 0,
    .audio_in_offset = 0,
    .log = stub_log,
    .midi_send_internal = NULL,
    .midi_send_external = NULL,
    .get_clock_status = NULL,
};
