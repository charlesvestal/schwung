/* shadow_led_queue.h - Rate-limited LED output queue
 * Extracted from schwung_shim.c for maintainability. */

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
/* Budget for restoring Move's LED state after overtake exit.
 * Higher than normal mode to restore quickly, but within safe limits. */
#define SHADOW_LED_RESTORE_BUDGET 16
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

/* Read the cached LED color for a note (from Move's MIDI_OUT).
 * Returns -1 if unknown, else the velocity/color value. */
int led_queue_get_note_led_color(int note);

/* JACK LED cache — track LED state from JACK MIDI output (note/CC) */
void led_queue_cache_jack_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2);
void led_queue_clear_jack_cache(void);
void led_queue_restore_jack_leds(void);

/* JACK sysex LED cache — cache RNBO's sysex LED commands for suspend/resume.
 * Call led_queue_jack_sysex_packet() for every USB-MIDI packet from JACK.
 * It reassembles sysex and caches LED color commands (3B 10 <idx> ...). */
void led_queue_jack_sysex_packet(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3);
void led_queue_clear_jack_sysex_cache(void);
void led_queue_restore_jack_sysex_leds(void);
int led_queue_jack_sysex_debug_info(int *starts, int *cached, int *last_cin);

#endif /* SHADOW_LED_QUEUE_H */
