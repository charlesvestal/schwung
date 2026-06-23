/*
 * commands.h — command registry + dispatch for schwung-testd.
 *
 * The daemon's command set lives entirely in commands.c, behind a verb
 * table. Adding a new command is one row in the table plus one handler
 * function — protocol.c stays untouched.
 *
 * Handlers all take (fd, args). They reply via protocol_reply() /
 * protocol_reply_err() and return:
 *    0 — normal: keep the connection open
 *    1 — close the connection after this reply (e.g. QUIT)
 *   <0 — internal error: connection will be dropped
 */

#ifndef SCHWUNG_TESTD_COMMANDS_H
#define SCHWUNG_TESTD_COMMANDS_H

#include "shadow_constants.h"

/* SHM pointers the daemon needs. main() sets these via commands_init()
 * before any client is served. */
typedef struct daemon_shm {
    shadow_control_t        *control;       /* RO: shim_counter for frame ack */
    shadow_midi_inject_t    *inject;        /* RW: producers' inject ring */
    shadow_overlay_state_t  *overlay;       /* RO: pad_led_colors snapshot */
    test_stream_shm_t       *midi_out_stream; /* RW: stream of MIDI_OUT events from shim */
    shadow_param_t          *param;         /* RW: get/set requests to chain DSPs / overtake modules */
} daemon_shm_t;

/* Wire SHM pointers into the command layer. Must be called before
 * commands_dispatch(). */
void commands_init(const daemon_shm_t *shm);

/* Reset per-client subscription state. Called on connection close so
 * the next client doesn't inherit a stale baseline from a dropped one. */
void commands_reset_client_state(void);

/* Dispatch a single command line to its handler.
 * Returns the handler's return code (see commands.h docblock above). */
int commands_dispatch(int fd, const char *verb, const char *args);

#endif /* SCHWUNG_TESTD_COMMANDS_H */
