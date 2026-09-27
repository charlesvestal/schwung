/* shadow_pin_scanner.h - PIN display scanner for screen reader
 * Extracted from schwung_shim.c for maintainability. */

#ifndef SHADOW_PIN_SCANNER_H
#define SHADOW_PIN_SCANNER_H

#include <stdbool.h>
#include <stdint.h>
#include "shadow_constants.h"

/* ============================================================================
 * Callback struct - shim functions PIN scanner needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    bool (*tts_speak)(const char *text);
    shadow_control_t *volatile *shadow_control;  /* Ptr to shim's shadow_control ptr */
} pin_scanner_host_t;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize PIN scanner with callbacks. Must be called before other functions. */
void pin_scanner_init(const pin_scanner_host_t *host);

/* Accumulate a display slice into the PIN display buffer.
 * Called from the ioctl handler's slice capture section.
 *
 * Returns 1 on the slice that COMPLETES a frame, so a caller that wants to
 * read Move's screen does not have to guess when one is whole. The buffer is
 * this module's; `pin_display_frame()` hands it out for the duration of that
 * call and it is overwritten by the next frame. */
int pin_accumulate_slice(int idx, const uint8_t *data, int bytes);

/* The accumulated frame: DISPLAY_BUFFER_SIZE bytes, 8 pages of 128 columns,
 * `frame[(row/8)*128 + col]` with the row's bit at `1 << (row % 8)`. Only
 * meaningful right after pin_accumulate_slice() returned 1. */
const uint8_t *pin_display_frame(void);

/* Main PIN scanner - called from the display section of the tick loop. */
void pin_check_and_speak(void);

#endif /* SHADOW_PIN_SCANNER_H */
