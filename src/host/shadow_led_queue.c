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

/* Move LED state cache — continuously accumulated from Move's MIDI_OUT.
 * When entering overtake we snapshot this, and on exit we restore it. */
static int move_note_led_state[128];         /* -1 = unknown, else color */
static uint8_t move_note_led_cin[128];
static uint8_t move_note_led_status[128];
static int move_cc_led_state[128];           /* -1 = unknown, else color */
static uint8_t move_cc_led_cin[128];
static uint8_t move_cc_led_status[128];

/* Snapshot taken at overtake entry — this is what we restore on exit */
static int snapshot_note_color[128];
static uint8_t snapshot_note_cin[128];
static uint8_t snapshot_note_status[128];
static int snapshot_cc_color[128];
static uint8_t snapshot_cc_cin[128];
static uint8_t snapshot_cc_status[128];
static int snapshot_valid = 0;

static int move_led_restore_pending = 0;
static int move_led_clear_pending = 0;
static int move_led_pass_count = 0;  /* how many clear/restore passes remain */
static int prev_overtake_mode = 0;

/* ============================================================================
 * Hardware LED indices — only target LEDs that physically exist on Move.
 * ============================================================================ */
static const int hw_note_leds[] = {
    /* Steps 1-16 */
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    /* Pads 1-32 */
    68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 97, 98, 99
};
#define HW_NOTE_LED_COUNT (sizeof(hw_note_leds) / sizeof(hw_note_leds[0]))

static const int hw_cc_leds[] = {
    /* NOTE: Track rows (40-43) excluded — snapshot captures wrong color.
     * Move doesn't re-assert them, so we leave them alone for now. */
    /* White LED buttons */
    49, 50, 51, 52, 54, 55, 56, 58, 60, 62, 63,
    /* Transport / record (RGB) */
    85, 86, 88, 118, 119
};
#define HW_CC_LED_COUNT (sizeof(hw_cc_leds) / sizeof(hw_cc_leds[0]))

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
    snapshot_valid = 0;
    move_led_restore_pending = 0;
    move_led_clear_pending = 0;
    move_led_pass_count = 0;
    prev_overtake_mode = 0;
    for (int i = 0; i < 128; i++) {
        move_note_led_state[i] = -1;
        move_cc_led_state[i] = -1;
        snapshot_note_color[i] = -1;
        snapshot_cc_color[i] = -1;
    }
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

/* Queue all-off for only the real hardware LEDs */
static void queue_hw_leds_off(void) {
    shadow_init_led_queue();
    for (int j = 0; j < (int)HW_NOTE_LED_COUNT; j++) {
        int i = hw_note_leds[j];
        shadow_pending_note_color[i] = 0;
        shadow_pending_note_status[i] = 0x90;
        shadow_pending_note_cin[i] = 0x09;
    }
    for (int j = 0; j < (int)HW_CC_LED_COUNT; j++) {
        int i = hw_cc_leds[j];
        shadow_pending_cc_color[i] = 0;
        shadow_pending_cc_status[i] = 0xB0;
        shadow_pending_cc_cin[i] = 0x0B;
    }
}

/* Queue snapshot restore for real hardware LEDs:
 * restore snapshotted color, or off if snapshot was -1 */
static void queue_hw_leds_restore(void) {
    shadow_init_led_queue();
    for (int j = 0; j < (int)HW_NOTE_LED_COUNT; j++) {
        int i = hw_note_leds[j];
        if (snapshot_note_color[i] >= 0) {
            shadow_pending_note_color[i] = snapshot_note_color[i];
            shadow_pending_note_status[i] = snapshot_note_status[i];
            shadow_pending_note_cin[i] = snapshot_note_cin[i];
        } else {
            shadow_pending_note_color[i] = 0;
            shadow_pending_note_status[i] = 0x90;
            shadow_pending_note_cin[i] = 0x09;
        }
    }
    for (int j = 0; j < (int)HW_CC_LED_COUNT; j++) {
        int i = hw_cc_leds[j];
        if (snapshot_cc_color[i] >= 0) {
            shadow_pending_cc_color[i] = snapshot_cc_color[i];
            shadow_pending_cc_status[i] = snapshot_cc_status[i];
            shadow_pending_cc_cin[i] = snapshot_cc_cin[i];
        } else {
            shadow_pending_cc_color[i] = 0;
            shadow_pending_cc_status[i] = 0xB0;
            shadow_pending_cc_cin[i] = 0x0B;
        }
    }
}

void shadow_clear_move_leds_if_overtake(void) {
    shadow_control_t *ctrl = host.shadow_control ? *host.shadow_control : NULL;
    int cur_overtake = (ctrl && ctrl->overtake_mode >= 2) ? 1 : 0;

    uint8_t *midi_out = host.midi_out_buf;
    if (!midi_out) {
        prev_overtake_mode = cur_overtake;
        return;
    }

    /* Scan Move's MIDI_OUT and cache LED state.
     * When not in overtake, always cache. When in overtake with skip_led_clear,
     * also cache since Move's LEDs are passing through and we want an up-to-date
     * snapshot for restore on exit. */
    if (!cur_overtake || (ctrl && ctrl->skip_led_clear)) {
        for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
            uint8_t cable = (midi_out[i] >> 4) & 0x0F;
            uint8_t type = midi_out[i+1] & 0xF0;
            if (cable == 0 && (type == 0x90 || type == 0xB0)) {
                uint8_t d1 = midi_out[i+2];
                uint8_t d2 = midi_out[i+3];
                if (type == 0x90) {
                    move_note_led_state[d1] = d2;
                    move_note_led_status[d1] = midi_out[i+1];
                    move_note_led_cin[d1] = midi_out[i];
                } else {
                    move_cc_led_state[d1] = d2;
                    move_cc_led_status[d1] = midi_out[i+1];
                    move_cc_led_cin[d1] = midi_out[i];
                }
            }
        }
    }

    /* On transition into overtake: snapshot LED state, then clear (or restore).
     * Two passes to catch any Move re-asserts between frames.
     * If skip_led_clear is set, restore the snapshot instead of clearing
     * so pad colors stay visible (e.g. song-mode). */
    if (!prev_overtake_mode && cur_overtake) {
        memcpy(snapshot_note_color, move_note_led_state, sizeof(snapshot_note_color));
        memcpy(snapshot_note_cin, move_note_led_cin, sizeof(snapshot_note_cin));
        memcpy(snapshot_note_status, move_note_led_status, sizeof(snapshot_note_status));
        memcpy(snapshot_cc_color, move_cc_led_state, sizeof(snapshot_cc_color));
        memcpy(snapshot_cc_cin, move_cc_led_cin, sizeof(snapshot_cc_cin));
        memcpy(snapshot_cc_status, move_cc_led_status, sizeof(snapshot_cc_status));
        snapshot_valid = 1;

        if (ctrl && ctrl->skip_led_clear) {
            /* Do nothing — LEDs are already correct and Move's MIDI_OUT
             * passes through during overtake with skip_led_clear.
             * No restore needed (avoids LED flicker from re-sending). */
            move_led_restore_pending = 0;
            move_led_clear_pending = 0;
            move_led_pass_count = 0;
        } else {
            queue_hw_leds_off();
            move_led_clear_pending = 1;
            move_led_restore_pending = 0;
            move_led_pass_count = 1;
        }
    }

    /* On transition out of overtake: restore from snapshot.
     * LEDs we captured get restored; unknowns get turned off.
     * Two passes to catch stragglers.
     * If skip_led_clear was active, LEDs have been passing through live
     * so Move's current state is already on hardware — no restore needed. */
    if (prev_overtake_mode && !cur_overtake && snapshot_valid) {
        if (!(ctrl && ctrl->skip_led_clear)) {
            queue_hw_leds_restore();
            move_led_restore_pending = 1;
            move_led_clear_pending = 0;
            move_led_pass_count = 1;
        }
    }

    prev_overtake_mode = cur_overtake;

    /* During overtake: clear Move's cable-0 LED packets from MIDI_OUT
     * so the overtake module has full LED control.
     * If skip_led_clear is set, let Move's LEDs pass through (e.g. song-mode
     * wants Move's pad colors to update as clips play). */
    if (!cur_overtake) return;
    if (ctrl && ctrl->skip_led_clear) return;

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
     * In normal mode stay within safe limit to coexist with Move's packets.
     * During restore, use a higher budget to get LEDs back quickly. */
    int max_bytes = overtake ? MIDI_BUFFER_SIZE : SHADOW_LED_QUEUE_SAFE_BYTES;
    int available = (max_bytes - used) / 4;
    int budget;
    if (overtake) {
        budget = SHADOW_LED_OVERTAKE_BUDGET;
    } else if (move_led_restore_pending || move_led_clear_pending) {
        budget = SHADOW_LED_RESTORE_BUDGET;
    } else {
        budget = SHADOW_LED_MAX_UPDATES_PER_TICK;
    }
    if (available <= 0 || budget <= 0) return;
    if (budget > available) budget = available;

    int sent = 0;
    int hw_offset = 0;

    /* First flush pending note-on messages */
    int notes_remaining = 0;
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
    for (int i = 0; i < 128; i++) {
        if (shadow_pending_note_color[i] >= 0) { notes_remaining = 1; break; }
    }

    /* Then flush pending CC messages */
    int ccs_remaining = 0;
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
    for (int i = 0; i < 128; i++) {
        if (shadow_pending_cc_color[i] >= 0) { ccs_remaining = 1; break; }
    }

    /* When a pass completes, either queue the next pass or clear the flags */
    if ((move_led_restore_pending || move_led_clear_pending) &&
        !notes_remaining && !ccs_remaining) {
        if (move_led_pass_count > 0) {
            move_led_pass_count--;
            /* Queue another pass of the same type */
            if (move_led_clear_pending) {
                queue_hw_leds_off();
            } else {
                queue_hw_leds_restore();
            }
        } else {
            move_led_restore_pending = 0;
            move_led_clear_pending = 0;
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

int led_queue_get_note_led_color(int note) {
    if (note < 0 || note >= 128) return -1;
    return move_note_led_state[note];
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
