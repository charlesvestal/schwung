/* shadow_led_queue.c - Rate-limited LED output queue
 * Extracted from schwung_shim.c for maintainability. */

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

/* Raw packet queue for sysex and other non-note/CC messages */
#define RAW_QUEUE_SIZE 256
static uint8_t raw_queue[RAW_QUEUE_SIZE][4];  /* 4 bytes per USB-MIDI packet */
static int raw_queue_head = 0;
static int raw_queue_tail = 0;

/* Move LED state cache — continuously accumulated from Move's MIDI_OUT.
 * When entering overtake we snapshot this, and on exit we restore it. */
static int move_note_led_state[128];         /* -1 = unknown, else color */
static uint8_t move_note_led_cin[128];
static uint8_t move_note_led_status[128];
static int move_cc_led_state[128];           /* -1 = unknown, else color */
static uint8_t move_cc_led_cin[128];
static uint8_t move_cc_led_status[128];

/* JACK LED state cache — continuously accumulated from JACK MIDI output.
 * On suspend we keep this intact; on resume we replay it to restore RNBO's LEDs. */
static int jack_note_led_state[128];
static uint8_t jack_note_led_cin[128];
static uint8_t jack_note_led_status[128];
static int jack_cc_led_state[128];
static uint8_t jack_cc_led_cin[128];
static uint8_t jack_cc_led_status[128];

/* Snapshot taken at overtake entry — this is what we restore on exit */
static int snapshot_note_color[128];
static uint8_t snapshot_note_cin[128];
static uint8_t snapshot_note_status[128];
static int snapshot_cc_color[128];
static uint8_t snapshot_cc_cin[128];
static uint8_t snapshot_cc_status[128];
static int snapshot_valid = 0;
static int snapshot_skip_restore = 0;  /* set when entering with skip_led_clear — skip restore on exit */

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
    /* Track row LEDs */
    40, 41, 42, 43,
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
    snapshot_skip_restore = 0;
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
    for (int i = 0; i < 128; i++) {
        jack_note_led_state[i] = -1;
        jack_cc_led_state[i] = -1;
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
        /* Note-on: queue by note number (last-writer-wins) */
        shadow_pending_note_color[data1] = data2;
        shadow_pending_note_status[data1] = status;
        shadow_pending_note_cin[data1] = cin;
    } else if (type == 0xB0) {
        /* CC: queue by CC number (last-writer-wins) */
        shadow_pending_cc_color[data1] = data2;
        shadow_pending_cc_status[data1] = status;
        shadow_pending_cc_cin[data1] = cin;
    } else {
        /* Sysex and other messages: FIFO queue */
        int next = (raw_queue_head + 1) % RAW_QUEUE_SIZE;
        if (next != raw_queue_tail) {
            raw_queue[raw_queue_head][0] = cin;
            raw_queue[raw_queue_head][1] = status;
            raw_queue[raw_queue_head][2] = data1;
            raw_queue[raw_queue_head][3] = data2;
            raw_queue_head = next;
        }
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

        snapshot_skip_restore = (ctrl && ctrl->skip_led_clear) ? 1 : 0;
        if (snapshot_skip_restore) {
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
     * If skip_led_clear was active at entry, LEDs have been passing through
     * live so Move's current state is already on hardware — no restore needed. */
    if (prev_overtake_mode && !cur_overtake && snapshot_valid) {
        if (!snapshot_skip_restore) {
            queue_hw_leds_restore();
            move_led_restore_pending = 1;
            move_led_clear_pending = 0;
            move_led_pass_count = 1;
        }
        snapshot_skip_restore = 0;
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
    int skip_led_clear = ctrl && ctrl->skip_led_clear;
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
    /* When skip_led_clear is active, Move's packets fill the buffer.
     * We can still replace matching packets, so don't bail on available<=0. */
    if (!skip_led_clear && (available <= 0 || budget <= 0)) return;
    if (budget <= 0) return;
    if (!skip_led_clear && budget > available) budget = available;

    int sent = 0;
    int hw_offset = 0;

    /* First flush pending note-on messages */
    int notes_remaining = 0;
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_pending_note_color[i] >= 0) {
            int slot = -1;
            /* When skip_led_clear is active, first try to replace Move's
             * existing packet for the same note (buffer may be full). */
            if (skip_led_clear) {
                for (int s = 0; s < MIDI_BUFFER_SIZE; s += 4) {
                    uint8_t type = midi_out[s+1] & 0xF0;
                    if (type == 0x90 && midi_out[s+2] == (uint8_t)i) {
                        slot = s;
                        break;
                    }
                }
            }
            /* Fall back to finding an empty slot */
            if (slot < 0) {
                while (hw_offset < MIDI_BUFFER_SIZE) {
                    if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                        midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                        break;
                    }
                    hw_offset += 4;
                }
                if (hw_offset < MIDI_BUFFER_SIZE) {
                    slot = hw_offset;
                    hw_offset += 4;
                }
            }
            if (slot < 0) break;  /* No slot found at all */

            midi_out[slot] = shadow_pending_note_cin[i];
            midi_out[slot+1] = shadow_pending_note_status[i];
            midi_out[slot+2] = (uint8_t)i;
            midi_out[slot+3] = (uint8_t)shadow_pending_note_color[i];
            shadow_pending_note_color[i] = -1;
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
            int slot = -1;
            if (skip_led_clear) {
                for (int s = 0; s < MIDI_BUFFER_SIZE; s += 4) {
                    uint8_t type = midi_out[s+1] & 0xF0;
                    if (type == 0xB0 && midi_out[s+2] == (uint8_t)i) {
                        slot = s;
                        break;
                    }
                }
            }
            if (slot < 0) {
                while (hw_offset < MIDI_BUFFER_SIZE) {
                    if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                        midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                        break;
                    }
                    hw_offset += 4;
                }
                if (hw_offset < MIDI_BUFFER_SIZE) {
                    slot = hw_offset;
                    hw_offset += 4;
                }
            }
            if (slot < 0) break;

            midi_out[slot] = shadow_pending_cc_cin[i];
            midi_out[slot+1] = shadow_pending_cc_status[i];
            midi_out[slot+2] = (uint8_t)i;
            midi_out[slot+3] = (uint8_t)shadow_pending_cc_color[i];
            shadow_pending_cc_color[i] = -1;
            sent++;
        }
    }
    for (int i = 0; i < 128; i++) {
        if (shadow_pending_cc_color[i] >= 0) { ccs_remaining = 1; break; }
    }

    /* Flush raw packet queue (sysex, etc.) */
    while (raw_queue_tail != raw_queue_head && sent < budget) {
        while (hw_offset < MIDI_BUFFER_SIZE) {
            if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_BUFFER_SIZE) break;

        midi_out[hw_offset]   = raw_queue[raw_queue_tail][0];
        midi_out[hw_offset+1] = raw_queue[raw_queue_tail][1];
        midi_out[hw_offset+2] = raw_queue[raw_queue_tail][2];
        midi_out[hw_offset+3] = raw_queue[raw_queue_tail][3];
        hw_offset += 4;
        raw_queue_tail = (raw_queue_tail + 1) % RAW_QUEUE_SIZE;
        sent++;
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

    /* Progressive sysex LED restore — 2 LEDs per tick (12 packets).
     * Writes directly to MIDI_OUT, bypassing the raw queue.
     * Limited to avoid filling the 20-slot SPI MIDI buffer. */
    led_queue_flush_jack_sysex_restore(2);
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

void led_queue_cache_jack_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        jack_note_led_state[data1] = data2;
        jack_note_led_status[data1] = status;
        jack_note_led_cin[data1] = cin;
    } else if (type == 0xB0) {
        jack_cc_led_state[data1] = data2;
        jack_cc_led_status[data1] = status;
        jack_cc_led_cin[data1] = cin;
    }
}

void led_queue_clear_jack_cache(void) {
    for (int i = 0; i < 128; i++) {
        jack_note_led_state[i] = -1;
        jack_cc_led_state[i] = -1;
    }
    led_queue_clear_jack_sysex_cache();
}

void led_queue_restore_jack_leds(void) {
    shadow_init_led_queue();
    for (int j = 0; j < (int)HW_NOTE_LED_COUNT; j++) {
        int i = hw_note_leds[j];
        if (jack_note_led_state[i] >= 0) {
            shadow_pending_note_color[i] = jack_note_led_state[i];
            shadow_pending_note_status[i] = jack_note_led_status[i];
            shadow_pending_note_cin[i] = jack_note_led_cin[i];
        }
    }
    for (int j = 0; j < (int)HW_CC_LED_COUNT; j++) {
        int i = hw_cc_leds[j];
        if (jack_cc_led_state[i] >= 0) {
            shadow_pending_cc_color[i] = jack_cc_led_state[i];
            shadow_pending_cc_status[i] = jack_cc_led_status[i];
            shadow_pending_cc_cin[i] = jack_cc_led_cin[i];
        }
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

/* ============================================================================
 * JACK Sysex LED Cache
 *
 * RNBO sends LED colors via Ableton sysex: F0 00 21 1D 01 01 3B 10 <idx> <rgb...> F7
 * These arrive as USB-MIDI packets (CIN 0x04 = sysex continue, 0x05-07 = sysex end).
 * We reassemble the sysex stream, detect LED color commands, and cache the raw
 * USB-MIDI packets per LED index. On resume, replay the cached packets.
 * ============================================================================ */

#define JACK_SYSEX_MAX_LEDS 128
#define JACK_SYSEX_PACKETS_PER_LED 6  /* 16 bytes = 6 USB-MIDI packets */

/* Each cached LED entry: 6 raw USB-MIDI packets (4 bytes each) */
typedef struct {
    uint8_t packets[JACK_SYSEX_PACKETS_PER_LED][4];
    int valid;
} jack_sysex_led_entry_t;

static jack_sysex_led_entry_t jack_sysex_led_cache[JACK_SYSEX_MAX_LEDS];

/* Sysex reassembly state */
static uint8_t sysex_buf[32];
static int sysex_buf_len = 0;
static int sysex_active = 0;
/* Raw packet accumulator for the current sysex message */
static uint8_t sysex_raw_packets[8][4];
static int sysex_raw_count = 0;

/* Debug counters for sysex cache */
static int sysex_packets_seen = 0;
static int sysex_starts_seen = 0;
static int sysex_leds_cached = 0;
static int sysex_last_cin = 0;

void led_queue_jack_sysex_packet(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t cin_type = cin & 0x0F;
    sysex_packets_seen++;
    sysex_last_cin = cin_type;

    /* CIN 0x04 = sysex start or continue (3 data bytes) */
    if (cin_type == 0x04) {
        if (b1 == 0xF0) {
            /* Sysex start */
            sysex_buf_len = 0;
            sysex_active = 1;
            sysex_raw_count = 0;
            sysex_starts_seen++;
        }
        if (sysex_active && sysex_buf_len + 3 <= (int)sizeof(sysex_buf)) {
            sysex_buf[sysex_buf_len++] = b1;
            sysex_buf[sysex_buf_len++] = b2;
            sysex_buf[sysex_buf_len++] = b3;
        }
        if (sysex_active && sysex_raw_count < 8) {
            sysex_raw_packets[sysex_raw_count][0] = cin;
            sysex_raw_packets[sysex_raw_count][1] = b1;
            sysex_raw_packets[sysex_raw_count][2] = b2;
            sysex_raw_packets[sysex_raw_count][3] = b3;
            sysex_raw_count++;
        }
    }
    /* CIN 0x05 = sysex end (1 byte), 0x06 = sysex end (2 bytes), 0x07 = sysex end (3 bytes) */
    else if (cin_type >= 0x05 && cin_type <= 0x07) {
        int end_bytes = cin_type - 0x04;  /* 1, 2, or 3 */
        if (sysex_active && sysex_buf_len + end_bytes <= (int)sizeof(sysex_buf)) {
            sysex_buf[sysex_buf_len++] = b1;
            if (end_bytes >= 2) sysex_buf[sysex_buf_len++] = b2;
            if (end_bytes >= 3) sysex_buf[sysex_buf_len++] = b3;
        }
        if (sysex_active && sysex_raw_count < 8) {
            sysex_raw_packets[sysex_raw_count][0] = cin;
            sysex_raw_packets[sysex_raw_count][1] = b1;
            sysex_raw_packets[sysex_raw_count][2] = b2;
            sysex_raw_packets[sysex_raw_count][3] = b3;
            sysex_raw_count++;
        }

        /* Check if this is an Ableton LED color sysex:
         * F0 00 21 1D 01 01 3B 10 <idx> <r_lo> <r_hi> <g_lo> <g_hi> <b_lo> <b_hi> F7
         * That's 16 bytes, 6 USB-MIDI packets */
        if (sysex_active && sysex_buf_len >= 16 &&
            sysex_buf[0] == 0xF0 && sysex_buf[1] == 0x00 && sysex_buf[2] == 0x21 &&
            sysex_buf[3] == 0x1D && sysex_buf[4] == 0x01 && sysex_buf[5] == 0x01 &&
            sysex_buf[6] == 0x3B && sysex_buf[7] == 0x10 &&
            sysex_raw_count == JACK_SYSEX_PACKETS_PER_LED) {
            uint8_t idx = sysex_buf[8];
            if (idx < JACK_SYSEX_MAX_LEDS) {
                for (int p = 0; p < JACK_SYSEX_PACKETS_PER_LED; p++) {
                    for (int b = 0; b < 4; b++) {
                        jack_sysex_led_cache[idx].packets[p][b] = sysex_raw_packets[p][b];
                    }
                }
                jack_sysex_led_cache[idx].valid = 1;
                sysex_leds_cached++;
            }
        }

        sysex_active = 0;
        sysex_buf_len = 0;
        sysex_raw_count = 0;
    }
}

void led_queue_clear_jack_sysex_cache(void) {
    for (int i = 0; i < JACK_SYSEX_MAX_LEDS; i++) {
        jack_sysex_led_cache[i].valid = 0;
    }
    sysex_active = 0;
    sysex_buf_len = 0;
    sysex_raw_count = 0;
}

int led_queue_jack_sysex_debug_info(int *starts, int *cached, int *last_cin) {
    if (starts) *starts = sysex_starts_seen;
    if (cached) *cached = sysex_leds_cached;
    if (last_cin) *last_cin = sysex_last_cin;
    return sysex_packets_seen;
}

/* Progressive sysex LED restore state */
static int sysex_restore_pending = 0;
static int sysex_restore_index = 0;  /* Next LED index to restore */

void led_queue_restore_jack_sysex_leds(void) {
    /* Start progressive restore — actual work done in flush function */
    sysex_restore_pending = 1;
    sysex_restore_index = 0;
}

/* Called from shadow_flush_pending_leds to progressively drain sysex restore.
 * Restores up to max_leds LEDs per call (each LED = 6 raw packets). */
int led_queue_flush_jack_sysex_restore(int max_leds) {
    if (!sysex_restore_pending) return 0;

    uint8_t *midi_out = host.midi_out_buf;
    if (!midi_out) return 0;

    int leds_sent = 0;
    int hw_offset = 0;  /* Scan position for empty slots */

    while (sysex_restore_index < JACK_SYSEX_MAX_LEDS && leds_sent < max_leds) {
        if (!jack_sysex_led_cache[sysex_restore_index].valid) {
            sysex_restore_index++;
            continue;
        }

        /* Write 6 packets for this LED, finding individual empty slots */
        int wrote_all = 1;
        for (int p = 0; p < JACK_SYSEX_PACKETS_PER_LED; p++) {
            /* Find next empty slot */
            while (hw_offset < MIDI_BUFFER_SIZE &&
                   (midi_out[hw_offset] || midi_out[hw_offset+1] ||
                    midi_out[hw_offset+2] || midi_out[hw_offset+3])) {
                hw_offset += 4;
            }
            if (hw_offset >= MIDI_BUFFER_SIZE) { wrote_all = 0; break; }

            midi_out[hw_offset]   = jack_sysex_led_cache[sysex_restore_index].packets[p][0];
            midi_out[hw_offset+1] = jack_sysex_led_cache[sysex_restore_index].packets[p][1];
            midi_out[hw_offset+2] = jack_sysex_led_cache[sysex_restore_index].packets[p][2];
            midi_out[hw_offset+3] = jack_sysex_led_cache[sysex_restore_index].packets[p][3];
            hw_offset += 4;
        }

        if (!wrote_all) break;  /* Buffer full, try next tick */

        sysex_restore_index++;
        leds_sent++;
    }

    if (sysex_restore_index >= JACK_SYSEX_MAX_LEDS) {
        sysex_restore_pending = 0;
    }

    return leds_sent;
}
