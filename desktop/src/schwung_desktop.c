/*
 * schwung_desktop.c — see schwung_desktop.h.
 *
 * The host_api_v1_t served here is deliberately SPARSE, and the NULLs are the
 * design rather than an unfinished port. plugin_api_v1.h's contract is that
 * every callback is guarded by its caller as `if (host->fn)`, and the
 * reserved[8] tail absorbs a module compiled against a longer header, so a
 * field we cannot answer is safer left NULL than answered with a fiction.
 *
 * What is NULL, and why:
 *
 *   midi_send_internal      LED and surface output. There is no surface.
 *   midi_inject_to_move     Injects into Move's MIDI_IN. There is no Move.
 *   slot_recv_channel       Slot registration is the shim's table; a desktop
 *                           instance is not slot-registered, and the contract
 *                           already defines -2 for that. Left NULL so callers
 *                           take their own "host doesn't expose it" branch.
 *   mod_emit_value/clear    Host-side modulation routing; the chain does its
 *                           own LFOs internally and needs nothing from us.
 *   mapped_memory           The SPI mailbox. Phase 1 fills a synthetic one so
 *                           line-input modules see the plugin's input bus;
 *                           until then, NULL and audio_in_offset unused.
 *
 * WHAT MUST NOT BE LEFT ZERO IS sample_rate. It is a plain int, so no
 * `if (host->fn)` guard covers it, and a module dividing 2*pi*f/0 produces
 * NaN and silence — with ENUM parameters still working, which reads as a
 * module defect rather than as a missing field. The probe harness lost real
 * time to exactly this (tools/probe, trap 1).
 */
#include "schwung_desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "host/plugin_api_v1.h"

struct schwung_desktop {
    void             *handle;
    plugin_api_v2_t  *api;
    void             *instance;
    host_api_v1_t     host;
    void            (*log)(const char *);

    double bpm;
    double beat;
    int    running;
};

/* The host callbacks are C function pointers with no context argument, so the
 * transport they read has to be reachable without one. A single chain per
 * process is the shape the plugin uses (one chain per plugin instance — see
 * the plan), but a DAW loads many instances into ONE process, so this is a
 * real limitation to close before shipping: the transport is per-process here
 * and should be per-instance. Recorded rather than hidden. */
static schwung_desktop_t *g_transport_owner = NULL;

static void desktop_log(const char *msg) {
    if (g_transport_owner && g_transport_owner->log) g_transport_owner->log(msg);
}

static float desktop_get_bpm(void) {
    return g_transport_owner ? (float)g_transport_owner->bpm : 120.0f;
}

static double desktop_get_beat_position(void) {
    /* < 0 means no transport. An LFO free-runs on that answer; returning 0
     * instead would pin every free-running modulator to a downbeat that is
     * not happening. */
    if (!g_transport_owner || !g_transport_owner->running) return -1.0;
    return g_transport_owner->beat;
}

static int desktop_get_clock_status(void) {
    if (!g_transport_owner) return MOVE_CLOCK_STATUS_UNAVAILABLE;
    return g_transport_owner->running ? MOVE_CLOCK_STATUS_RUNNING
                                      : MOVE_CLOCK_STATUS_STOPPED;
}

schwung_desktop_t *schwung_desktop_create(const char *module_root,
                                          void (*log)(const char *))
{
    if (!module_root) return NULL;

    schwung_desktop_t *sd = calloc(1, sizeof(*sd));
    if (!sd) return NULL;
    sd->log = log;
    sd->bpm = 120.0;
    sd->beat = -1.0;
    sd->running = 0;
    g_transport_owner = sd;

    char chain_dir[1024];
    char chain_so[1152];
    snprintf(chain_dir, sizeof(chain_dir), "%s/chain", module_root);
    snprintf(chain_so, sizeof(chain_so), "%s/dsp.so", chain_dir);

    sd->handle = dlopen(chain_so, RTLD_NOW | RTLD_LOCAL);
    if (!sd->handle) {
        if (log) { char m[1400]; snprintf(m, sizeof(m), "dlopen %s: %s", chain_so, dlerror()); log(m); }
        free(sd); g_transport_owner = NULL; return NULL;
    }

    typedef plugin_api_v2_t *(*init_v2_fn)(const host_api_v1_t *);
    init_v2_fn init_v2 = (init_v2_fn)dlsym(sd->handle, "move_plugin_init_v2");
    if (!init_v2) {
        if (log) log("chain/dsp.so exports no move_plugin_init_v2");
        dlclose(sd->handle); free(sd); g_transport_owner = NULL; return NULL;
    }

    /* Zeroed by calloc, so every field not set below is NULL or 0 by
     * construction — including reserved[8], which is what makes an over-read
     * from a module built against a longer header find NULL. */
    sd->host.api_version      = 1;
    sd->host.sample_rate      = SCHWUNG_RATE;    /* never leave this 0 */
    sd->host.frames_per_block = SCHWUNG_BLOCK;
    sd->host.log              = desktop_log;
    sd->host.get_bpm          = desktop_get_bpm;
    sd->host.get_beat_position = desktop_get_beat_position;
    sd->host.get_clock_status = desktop_get_clock_status;

    sd->api = init_v2(&sd->host);
    if (!sd->api || sd->api->api_version != 2) {
        if (log) log("chain move_plugin_init_v2 returned no usable v2 api");
        dlclose(sd->handle); free(sd); g_transport_owner = NULL; return NULL;
    }

    sd->instance = sd->api->create_instance(chain_dir, "{}");
    if (!sd->instance) {
        if (log) log("chain create_instance returned NULL");
        dlclose(sd->handle); free(sd); g_transport_owner = NULL; return NULL;
    }
    return sd;
}

void schwung_desktop_destroy(schwung_desktop_t *sd) {
    if (!sd) return;
    if (sd->api && sd->api->destroy_instance && sd->instance)
        sd->api->destroy_instance(sd->instance);
    /* The handle is deliberately NOT dlclose'd. A chain that has loaded a C++
     * module carrying STB_GNU_UNIQUE symbols cannot be unmapped anyway
     * (dlclose is a no-op there), and unmapping the chain while a sub-module's
     * static destructors are still registered against it is how a teardown
     * turns into a crash. Leaking one handle per plugin instance is the
     * cheaper failure. */
    if (g_transport_owner == sd) g_transport_owner = NULL;
    free(sd);
}

void schwung_desktop_set_param(schwung_desktop_t *sd, const char *key, const char *val) {
    if (sd && sd->api && sd->api->set_param) sd->api->set_param(sd->instance, key, val);
}

int schwung_desktop_get_param(schwung_desktop_t *sd, const char *key, char *buf, int buf_len) {
    if (!sd || !sd->api || !sd->api->get_param) return -1;
    return sd->api->get_param(sd->instance, key, buf, buf_len);
}

void schwung_desktop_midi(schwung_desktop_t *sd, const uint8_t *msg, int len) {
    if (sd && sd->api && sd->api->on_midi)
        sd->api->on_midi(sd->instance, msg, len, MOVE_MIDI_SOURCE_INTERNAL);
}

void schwung_desktop_render(schwung_desktop_t *sd, int16_t *out_lr) {
    if (!sd || !sd->api || !sd->api->render_block) {
        if (out_lr) memset(out_lr, 0, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
        return;
    }
    /* render_block ACCUMULATES in some module paths, so the caller's buffer is
     * cleared here rather than trusted. */
    memset(out_lr, 0, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
    sd->api->render_block(sd->instance, out_lr, SCHWUNG_BLOCK);
}

void schwung_desktop_set_transport(schwung_desktop_t *sd, double bpm,
                                   double beat_position, int running)
{
    if (!sd) return;
    sd->bpm = (bpm > 0.0) ? bpm : 120.0;
    sd->beat = beat_position;
    sd->running = running;
}
