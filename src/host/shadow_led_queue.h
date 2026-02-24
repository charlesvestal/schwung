/* shadow_led_queue.h - Rate-limited LED output queue
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_LED_QUEUE_H
#define SHADOW_LED_QUEUE_H

#include <stdint.h>
#include "shadow_constants.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SHADOW_LED_MAX_UPDATES_PER_TICK 16
#define SHADOW_LED_QUEUE_SAFE_BYTES 76
/* In overtake mode we clear Move's cable-0 LEDs, freeing most of the buffer */
#define SHADOW_LED_OVERTAKE_BUDGET 48
/* Max input LED commands per tick from external devices */
#define SHADOW_INPUT_LED_MAX_PER_TICK 24

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    uint8_t *midi_out_buf;             /* shadow_mailbox + MIDI_OUT_OFFSET (static) */
    shadow_control_t *volatile *shadow_control;  /* Ptr to shim's shadow_control ptr */
    uint8_t *volatile *shadow_ui_midi_shm;       /* Ptr to shim's shadow_ui_midi_shm ptr */
} led_queue_host_t;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize LED queue module with host pointers. */
void led_queue_init(const led_queue_host_t *host);

/* Initialize pending LED queue arrays (idempotent). */
void shadow_init_led_queue(void);

/* Queue an LED update for rate-limited sending. */
void shadow_queue_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2);

/* In overtake mode, clear Move's cable-0 LED packets from MIDI_OUT buffer. */
void shadow_clear_move_leds_if_overtake(void);

/* Flush pending LED updates to hardware, rate-limited. */
void shadow_flush_pending_leds(void);

/* Queue an incoming LED command (cable 2 note-on) for rate-limited forwarding. */
void shadow_queue_input_led(uint8_t cin, uint8_t status, uint8_t note, uint8_t velocity);

/* Flush pending input LED commands to UI MIDI buffer, rate-limited. */
void shadow_flush_pending_input_leds(void);

#endif /* SHADOW_LED_QUEUE_H */
