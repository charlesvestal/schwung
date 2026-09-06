// boot_select_core.h — pure logic for boot-select, unit-tested on the host.
#ifndef BOOT_SELECT_CORE_H
#define BOOT_SELECT_CORE_H
#include <stdint.h>
#include <stddef.h>

#define BS_MIDI_IN_BYTES (31 * 8)   /* 31 events x 8 bytes; NEVER 256 */

typedef struct {
    int frames_seen;     /* 0 until first scan completes; edges suppressed at 0 */
    int back_down;
    int click_down;
} bs_input_state_t;

typedef struct {
    int back_pressed;    /* press edge this frame */
    int click_pressed;
    int jog_delta;       /* summed signed detents this frame */
} bs_input_events_t;

void bs_input_scan(bs_input_state_t *st, const uint8_t *src,
                   bs_input_events_t *out);

int bs_json_field(const char *buf, const char *key, char *out, size_t outlen);

typedef struct { char id[64]; char name[64]; } bs_row_t;
int bs_build_rows(const char *dir, bs_row_t *rows, int max);

#endif
