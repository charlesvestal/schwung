/* shadow_process.h - Shadow UI and Link subscriber process management
 * Extracted from schwung_shim.c for maintainability. */

#ifndef SHADOW_PROCESS_H
#define SHADOW_PROCESS_H

#include <stdint.h>
#include <sys/types.h>
#include "link_audio.h"

/* ============================================================================
 * Callback struct - shim functions process management needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    float (*get_bpm)(void *source_out);  /* sampler_get_bpm(tempo_source_t*) - uses void* to avoid header dep */
    link_audio_state_t *link_audio;      /* Link audio state for monitor thread */
} process_host_t;

/* ============================================================================
 * Extern globals - process state readable/writable by the shim
 * ============================================================================ */

/* Link subscriber state */
extern volatile int link_sub_started;
extern volatile pid_t link_sub_pid;
extern volatile uint32_t link_sub_ever_received;
extern volatile int link_sub_restart_count;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize process management with callbacks to shim functions.
 * Must be called before any other process management function. */
void process_init(const process_host_t *host);

/* Shadow UI process management */
void launch_shadow_ui(void);

/* Clear the rapid-relaunch backoff. Call when the user explicitly asks for the
 * shadow UI (shortcut press), so an earlier crash loop doesn't leave it
 * permanently unavailable. */
void launch_shadow_ui_reset_backoff(void);

/* True once launch_shadow_ui() has stopped relaunching a repeatedly-dying
 * shadow_ui. Read off the SPI path (the launcher itself cannot log). */
int shadow_ui_relaunch_backoff_active(void);

/* Link subscriber process management */
void launch_link_subscriber(void);
void start_link_sub_monitor(void);
void link_sub_kill(void);
void link_sub_reset_state(void);

#endif /* SHADOW_PROCESS_H */
