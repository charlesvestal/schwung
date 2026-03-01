/* shadow_dbus.h - D-Bus monitoring, screen reader injection, volume/mute sync
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_DBUS_H
#define SHADOW_DBUS_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_PENDING_ANNOUNCEMENTS 4
#define MAX_ANNOUNCEMENT_LEN 8192
#define TTS_PRIORITY_BLOCK_MS 1000
#define OVERLAY_KNOBS_NATIVE 3

/* ============================================================================
 * Callback struct - shim functions D-Bus needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*save_state)(void);
    void (*apply_mute)(int slot, int is_muted);
    void (*ui_state_update_slot)(int slot);
    void (*native_sampler_update)(const char *text);
    /* Shared state pointers */
    shadow_chain_slot_t *chain_slots;
    shadow_control_t **shadow_control_ptr;
    uint8_t *display_mode;
    volatile int *held_track;
    volatile int *selected_slot;
    volatile int *solo_count;
    shadow_screenreader_t **screenreader_shm;
} dbus_host_t;

/* ============================================================================
 * Extern globals - D-Bus state readable/writable by the shim
 * ============================================================================ */

/* Native overlay knob mapping state (written by D-Bus handler, read by ioctl) */
extern volatile int8_t  native_knob_slot[8];
extern volatile uint8_t native_knob_touched[8];
extern volatile int     native_knob_any_touched;
extern volatile uint8_t native_knob_mapped[8];

/* Set overview detection (written by D-Bus handler) */
extern volatile int in_set_overview;

/* Priority announcement blocking (shared with TTS subsystem) */
extern bool tts_priority_announcement_active;
extern uint64_t tts_priority_announcement_time_ms;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize D-Bus subsystem with callbacks to shim functions.
 * Must be called before any other D-Bus function. */
void dbus_init(const dbus_host_t *host);

/* Start/stop D-Bus monitoring thread */
void shadow_dbus_start(void);
void shadow_dbus_stop(void);

/* Queue a screen reader announcement to be injected via send() hook */
void send_screenreader_announcement(const char *text);

/* Inject pending announcements via D-Bus FD write */
void shadow_inject_pending_announcements(void);

/* Hook callbacks - called from shim's LD_PRELOAD hooks */

/* Called from connect() hook after successful AF_UNIX connection */
void dbus_on_connect(int sockfd, const char *sun_path);

/* Called from send() hook. Returns 1 if this was a D-Bus send (result stored
 * in *result_out), 0 if not D-Bus (caller should pass through). */
int dbus_on_send(int sockfd, const void *buf, size_t len, int flags,
                 ssize_t (*real_send)(int, const void *, size_t, int),
                 ssize_t *result_out);

/* Called from sd_bus_default_system() hook after successful call */
void dbus_on_sd_bus_default(void *bus_ptr);

/* Called from sd_bus_start() hook after successful call */
void dbus_on_sd_bus_start(void *bus_ptr);

/* Utility: parse "Track Volume X dB" to linear float */
float shadow_parse_volume_db(const char *text);

#endif /* SHADOW_DBUS_H */
