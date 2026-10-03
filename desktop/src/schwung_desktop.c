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
 * mapped_memory is NOT in that list, and must never be: it is a pointer to
 * DATA rather than a callback, so nothing guards it. See the mailbox field.
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

/* The SPI transfer is 768 bytes mmap'd to 4096. Nothing here talks to any
 * hardware, but the region has to EXIST -- see the mailbox comment below. */
#define SCHWUNG_MAILBOX_BYTES 4096

struct schwung_desktop {
    void             *handle;
    plugin_api_v2_t  *api;
    void             *instance;
    host_api_v1_t     host;
    void            (*log)(const char *);

    double bpm;
    double beat;
    int    running;

    /* A SYNTHETIC SPI MAILBOX, AND IT IS NOT OPTIONAL.
     *
     * mapped_memory was NULL here at first, on the reasoning that a desktop
     * host has no mailbox and the field is guarded like every other. It is
     * not: it is a POINTER TO DATA, not a callback, so `if (host->fn)` never
     * covers it, and a module that reads its audio-in region does not check.
     * schwung-vocoder SIGSEGVs on the first render_block -- which in a DAW is
     * not a silent module, it is Live going down with the project unsaved.
     *
     * Same shape of trap as sample_rate being a plain int (see above): the
     * "every module guards its host access" reasoning only ever covered the
     * function pointers.
     *
     * Zeroed, so a line-input module reads silence and behaves as though
     * nothing is plugged in. Feeding the plugin's actual input bus into
     * audio_in_offset is the next step and makes vocoder, talkbox, breath and
     * the rest genuinely work; this much stops them crashing. */
    uint8_t mailbox[SCHWUNG_MAILBOX_BYTES];
};

/*
 * WHICH INSTANCE IS THE CALLBACK ASKING ABOUT?
 *
 * host_api_v1_t's callbacks are plain C function pointers with NO context
 * argument -- get_bpm(void), get_beat_position(void). On the device that is
 * fine, because there is exactly one host. In a DAW there is one per track,
 * and a file-static "current instance" means track 2's tempo answers track
 * 1's question: two Schwung tracks, one of them silently synced to the
 * other's transport, with nothing to see in either.
 *
 * These callbacks only ever run SYNCHRONOUSLY from inside one of the
 * schwung_desktop_* entry points below -- the chain calls them from
 * render_block, set_param, get_param and on_midi and nowhere else -- so the
 * instance is published in a THREAD-LOCAL around each of those calls.
 *
 * Thread-local rather than global because a DAW renders its tracks on several
 * worker threads at once, which is precisely the case a single global gets
 * wrong. Saved and restored rather than assigned, so a nested call (a chain
 * sub-plugin reaching back in) cannot strand the wrong instance.
 */
static _Thread_local schwung_desktop_t *g_current = NULL;

#define SD_ENTER(sd)  schwung_desktop_t *sd_prev_ = g_current; g_current = (sd)
#define SD_LEAVE()    g_current = sd_prev_

static void desktop_log(const char *msg) {
    if (g_current && g_current->log) g_current->log(msg);
}

static float desktop_get_bpm(void) {
    return g_current ? (float)g_current->bpm : 120.0f;
}

static double desktop_get_beat_position(void) {
    /* < 0 means no transport. An LFO free-runs on that answer; returning 0
     * instead would pin every free-running modulator to a downbeat that is
     * not happening. */
    if (!g_current || !g_current->running) return -1.0;
    return g_current->beat;
}

static int desktop_get_clock_status(void) {
    if (!g_current) return MOVE_CLOCK_STATUS_UNAVAILABLE;
    return g_current->running ? MOVE_CLOCK_STATUS_RUNNING
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
    char chain_dir[1024];
    char chain_so[1152];
    snprintf(chain_dir, sizeof(chain_dir), "%s/chain", module_root);
    snprintf(chain_so, sizeof(chain_so), "%s/dsp.so", chain_dir);

    sd->handle = dlopen(chain_so, RTLD_NOW | RTLD_LOCAL);
    if (!sd->handle) {
        if (log) { char m[1400]; snprintf(m, sizeof(m), "dlopen %s: %s", chain_so, dlerror()); log(m); }
        free(sd); return NULL;
    }

    typedef plugin_api_v2_t *(*init_v2_fn)(const host_api_v1_t *);
    init_v2_fn init_v2 = (init_v2_fn)dlsym(sd->handle, "move_plugin_init_v2");
    if (!init_v2) {
        if (log) log("chain/dsp.so exports no move_plugin_init_v2");
        dlclose(sd->handle); free(sd); return NULL;
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
    sd->host.mapped_memory    = sd->mailbox;      /* never NULL -- see above */
    sd->host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    sd->host.audio_in_offset  = MOVE_AUDIO_IN_OFFSET;

    sd->api = init_v2(&sd->host);
    if (!sd->api || sd->api->api_version != 2) {
        if (log) log("chain move_plugin_init_v2 returned no usable v2 api");
        dlclose(sd->handle); free(sd); return NULL;
    }

    SD_ENTER(sd);
    sd->instance = sd->api->create_instance(chain_dir, "{}");
    SD_LEAVE();
    if (!sd->instance) {
        if (log) log("chain create_instance returned NULL");
        dlclose(sd->handle); free(sd); return NULL;
    }
    return sd;
}

void schwung_desktop_destroy(schwung_desktop_t *sd) {
    if (!sd) return;
    if (sd->api && sd->api->destroy_instance && sd->instance) {
        SD_ENTER(sd);
        sd->api->destroy_instance(sd->instance);
        SD_LEAVE();
    }
    /* The handle is deliberately NOT dlclose'd. A chain that has loaded a C++
     * module carrying STB_GNU_UNIQUE symbols cannot be unmapped anyway
     * (dlclose is a no-op there), and unmapping the chain while a sub-module's
     * static destructors are still registered against it is how a teardown
     * turns into a crash. Leaking one handle per plugin instance is the
     * cheaper failure. */
    free(sd);
}

void schwung_desktop_set_param(schwung_desktop_t *sd, const char *key, const char *val) {
    if (!sd || !sd->api || !sd->api->set_param) return;
    SD_ENTER(sd);
    sd->api->set_param(sd->instance, key, val);
    SD_LEAVE();
}

int schwung_desktop_get_param(schwung_desktop_t *sd, const char *key, char *buf, int buf_len) {
    if (!sd || !sd->api || !sd->api->get_param) return -1;
    SD_ENTER(sd);
    int n = sd->api->get_param(sd->instance, key, buf, buf_len);
    SD_LEAVE();
    return n;
}

void schwung_desktop_midi(schwung_desktop_t *sd, const uint8_t *msg, int len) {
    if (!sd || !sd->api || !sd->api->on_midi) return;
    SD_ENTER(sd);
    sd->api->on_midi(sd->instance, msg, len, MOVE_MIDI_SOURCE_INTERNAL);
    SD_LEAVE();
}

void schwung_desktop_render(schwung_desktop_t *sd, int16_t *out_lr) {
    if (!sd || !sd->api || !sd->api->render_block) {
        if (out_lr) memset(out_lr, 0, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
        return;
    }
    /* render_block ACCUMULATES in some module paths, so the caller's buffer is
     * cleared here rather than trusted. */
    memset(out_lr, 0, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
    SD_ENTER(sd);
    sd->api->render_block(sd->instance, out_lr, SCHWUNG_BLOCK);
    SD_LEAVE();
}

void schwung_desktop_set_audio_in(schwung_desktop_t *sd, const int16_t *in_lr)
{
    if (!sd) return;
    int16_t *dst = (int16_t *)(sd->mailbox + MOVE_AUDIO_IN_OFFSET);
    if (in_lr) memcpy(dst, in_lr, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
    else       memset(dst, 0, sizeof(int16_t) * SCHWUNG_BLOCK * 2);
}

void schwung_desktop_set_transport(schwung_desktop_t *sd, double bpm,
                                   double beat_position, int running)
{
    if (!sd) return;
    sd->bpm = (bpm > 0.0) ? bpm : 120.0;
    sd->beat = beat_position;
    sd->running = running;
}
