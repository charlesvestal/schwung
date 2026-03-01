/* shadow_led_queue.c - Rate-limited LED output queue
 * Extracted from move_anything_shim.c for maintainability. */

#include <string.h>
#include "shadow_led_queue.h"

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static led_queue_host_t host;
static int led_queue_module_initialized = 0;

/* ============================================================================
 * Internal state
 * ============================================================================ */

/* Output LED queue */
static int shadow_pending_note_color[128];   /* -1 = not pending */
static uint8_t shadow_pending_note_status[128];
static uint8_t shadow_pending_note_cin[128];
static int shadow_pending_cc_color[128];     /* -1 = not pending */
static uint8_t shadow_pending_cc_status[128];
static uint8_t shadow_pending_cc_cin[128];
static int shadow_led_queue_initialized = 0;

/* Input LED queue (external MIDI cable 2) */
static int shadow_input_pending_note_color[128];   /* -1 = not pending */
static uint8_t shadow_input_pending_note_status[128];
static uint8_t shadow_input_pending_note_cin[128];
static int shadow_input_queue_initialized = 0;

/* ============================================================================
 * Init
 * ============================================================================ */

void led_queue_init(const led_queue_host_t *h) {
    host = *h;
    shadow_led_queue_initialized = 0;
    shadow_input_queue_initialized = 0;
    led_queue_module_initialized = 1;
}

/* ============================================================================
 * Output LED queue
 * ============================================================================ */

void shadow_init_led_queue(void) {
    if (shadow_led_queue_initialized) return;
    for (int i = 0; i < 128; i++) {
        shadow_pending_note_color[i] = -1;
        shadow_pending_cc_color[i] = -1;
    }
    shadow_led_queue_initialized = 1;
}

void shadow_queue_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        /* Note-on: queue by note number */
        shadow_pending_note_color[data1] = data2;
        shadow_pending_note_status[data1] = status;
        shadow_pending_note_cin[data1] = cin;
    } else if (type == 0xB0) {
        /* CC: queue by CC number */
        shadow_pending_cc_color[data1] = data2;
        shadow_pending_cc_status[data1] = status;
        shadow_pending_cc_cin[data1] = cin;
    }
}

void shadow_clear_move_leds_if_overtake(void) {
    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    if (!ctrl || ctrl->overtake_mode < 2) return;

    uint8_t *midi_out = host.midi_out_buf;
    if (!midi_out) return;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cable = (midi_out[i] >> 4) & 0x0F;
        uint8_t type = midi_out[i+1] & 0xF0;
        if (cable == 0 && (type == 0x90 || type == 0xB0)) {
            midi_out[i] = 0;
            midi_out[i+1] = 0;
            midi_out[i+2] = 0;
            midi_out[i+3] = 0;
        }
    }
}

void shadow_flush_pending_leds(void) {
    shadow_init_led_queue();

    uint8_t *midi_out = host.midi_out_buf;
    if (!midi_out) return;

    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    int overtake = ctrl && ctrl->overtake_mode >= 2;

    /* Count how many slots are already used */
    int used = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        if (midi_out[i] != 0 || midi_out[i+1] != 0 ||
            midi_out[i+2] != 0 || midi_out[i+3] != 0) {
            used += 4;
        }
    }

    /* In overtake mode use full buffer (after clearing Move's LEDs).
     * In normal mode stay within safe limit to coexist with Move's packets. */
    int max_bytes = overtake ? MIDI_BUFFER_SIZE : SHADOW_LED_QUEUE_SAFE_BYTES;
    int available = (max_bytes - used) / 4;
    int budget = overtake ? SHADOW_LED_OVERTAKE_BUDGET : SHADOW_LED_MAX_UPDATES_PER_TICK;
    if (available <= 0 || budget <= 0) return;
    if (budget > available) budget = available;

    int sent = 0;
    int hw_offset = 0;

    /* First flush pending note-on messages */
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_pending_note_color[i] >= 0) {
            /* Find empty slot */
            while (hw_offset < MIDI_BUFFER_SIZE) {
                if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                    midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                    break;
                }
                hw_offset += 4;
            }
            if (hw_offset >= MIDI_BUFFER_SIZE) break;

            midi_out[hw_offset] = shadow_pending_note_cin[i];
            midi_out[hw_offset+1] = shadow_pending_note_status[i];
            midi_out[hw_offset+2] = (uint8_t)i;
            midi_out[hw_offset+3] = (uint8_t)shadow_pending_note_color[i];
            shadow_pending_note_color[i] = -1;
            hw_offset += 4;
            sent++;
        }
    }

    /* Then flush pending CC messages */
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_pending_cc_color[i] >= 0) {
            /* Find empty slot */
            while (hw_offset < MIDI_BUFFER_SIZE) {
                if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                    midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                    break;
                }
                hw_offset += 4;
            }
            if (hw_offset >= MIDI_BUFFER_SIZE) break;

            midi_out[hw_offset] = shadow_pending_cc_cin[i];
            midi_out[hw_offset+1] = shadow_pending_cc_status[i];
            midi_out[hw_offset+2] = (uint8_t)i;
            midi_out[hw_offset+3] = (uint8_t)shadow_pending_cc_color[i];
            shadow_pending_cc_color[i] = -1;
            hw_offset += 4;
            sent++;
        }
    }
}

/* ============================================================================
 * Input LED queue (external MIDI cable 2)
 * ============================================================================ */

static void shadow_init_input_led_queue(void) {
    if (shadow_input_queue_initialized) return;
    for (int i = 0; i < 128; i++) {
        shadow_input_pending_note_color[i] = -1;
    }
    shadow_input_queue_initialized = 1;
}

void shadow_queue_input_led(uint8_t cin, uint8_t status, uint8_t note, uint8_t velocity) {
    shadow_init_input_led_queue();
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        shadow_input_pending_note_color[note] = velocity;
        shadow_input_pending_note_status[note] = status;
        shadow_input_pending_note_cin[note] = cin;
    }
}

void shadow_flush_pending_input_leds(void) {
    uint8_t *ui_midi = host.shadow_ui_midi_shm ? *host.shadow_ui_midi_shm : NULL;
    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    if (!ui_midi || !ctrl) return;
    shadow_init_input_led_queue();

    int budget = SHADOW_INPUT_LED_MAX_PER_TICK;
    int sent = 0;

    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_input_pending_note_color[i] >= 0) {
            /* Find empty slot in UI MIDI buffer */
            int found = 0;
            for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                if (ui_midi[slot] == 0) {
                    ui_midi[slot] = shadow_input_pending_note_cin[i];
                    ui_midi[slot + 1] = shadow_input_pending_note_status[i];
                    ui_midi[slot + 2] = (uint8_t)i;
                    ui_midi[slot + 3] = (uint8_t)shadow_input_pending_note_color[i];
                    ctrl->midi_ready++;
                    found = 1;
                    break;
                }
            }
            if (!found) break;  /* Buffer full, try again next tick */
            shadow_input_pending_note_color[i] = -1;
            sent++;
        }
    }
}
