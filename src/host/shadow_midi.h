/* shadow_midi.h - MIDI routing, dispatch, and forwarding
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_MIDI_H
#define SHADOW_MIDI_H

#include <stdint.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"
#include "plugin_api_v1.h"

/* ============================================================================
 * Move Hardware CC Constants
 * ============================================================================ */

#ifndef CC_SHIFT
#define CC_SHIFT 49
#define CC_JOG_CLICK 3
#define CC_JOG_WHEEL 14
#define CC_BACK 51
#define CC_MASTER_KNOB 79
#define CC_UP 55
#define CC_DOWN 54
#define CC_MENU 50
#define CC_CAPTURE 52
#define CC_UNDO 56
#define CC_LOOP 58
#define CC_COPY 60
#define CC_LEFT 62
#define CC_RIGHT 63
#define CC_KNOB1 71
#define CC_KNOB2 72
#define CC_KNOB3 73
#define CC_KNOB4 74
#define CC_KNOB5 75
#define CC_KNOB6 76
#define CC_KNOB7 77
#define CC_KNOB8 78
#define CC_PLAY 85
#define CC_REC 86
#define CC_SAMPLE 87
#define CC_MUTE 88
#define CC_MIC_IN_DETECT 114
#define CC_LINE_OUT_DETECT 115
#define CC_RECORD 118
#define CC_DELETE 119
#define CC_STEP_UI_FIRST 16
#define CC_STEP_UI_LAST 31
#endif

/* ============================================================================
 * Mailbox Layout Constants
 * ============================================================================ */

#ifndef MIDI_OUT_OFFSET
#define MAILBOX_SIZE 4096
#define MIDI_OUT_OFFSET 0
#define AUDIO_OUT_OFFSET 256
#define DISPLAY_OFFSET 768
#define MIDI_IN_OFFSET 2048
#define AUDIO_IN_OFFSET 2304
#define AUDIO_BUFFER_SIZE 512
#endif

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*midi_out_logf)(const char *fmt, ...);
    int (*midi_out_log_enabled)(void);
    void (*ui_state_update_slot)(int slot);
    void (*master_fx_forward_midi)(const uint8_t *msg, int len, int source);
    void (*queue_led)(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2);
    void (*init_led_queue)(void);
    /* Shared state */
    shadow_chain_slot_t *chain_slots;
    const plugin_api_v2_t *volatile *plugin_v2;
    shadow_control_t *volatile *shadow_control;
    unsigned char **global_mmap_addr;
    int *shadow_inprocess_ready;
    uint8_t *shadow_display_mode;
    /* SHM segment pointers */
    uint8_t **shadow_midi_shm;
    shadow_midi_out_t **shadow_midi_out_shm;
    uint8_t **shadow_ui_midi_shm;
    shadow_midi_dsp_t **shadow_midi_dsp_shm;
    uint8_t *shadow_mailbox;
    /* Capture state */
    shadow_capture_rules_t *master_fx_capture;
    /* Per-slot idle tracking */
    int *slot_idle;
    int *slot_silence_frames;
    int *slot_fx_idle;
    int *slot_fx_silence_frames;
} midi_host_t;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize MIDI routing module with host pointers. */
void midi_routing_init(const midi_host_t *host);

/* Remap MIDI channel for a specific slot based on forward_channel config. */
uint8_t shadow_chain_remap_channel(int slot, uint8_t status);

/* Dispatch a USB-MIDI packet to matching chain slots (by channel). */
void shadow_chain_dispatch_midi_to_slots(const uint8_t *pkt, int log_on, int *midi_log_count);

/* Forward CC/pitch bend/aftertouch from external MIDI (cable 2) into MIDI_OUT. */
void shadow_forward_external_cc_to_out(void);

/* Inject shadow UI MIDI output into the mailbox before ioctl. */
void shadow_inject_ui_midi_out(void);

/* Drain MIDI-to-DSP buffer from shadow UI and dispatch to chain slots. */
void shadow_drain_ui_midi_dsp(void);

/* Copy incoming MIDI from mailbox to shadow shared memory. */
void shadow_forward_midi(void);

/* Get capture rules for the focused slot (0-3 = chain, 4 = master FX). */
const shadow_capture_rules_t *shadow_get_focused_capture(void);

#endif /* SHADOW_MIDI_H */
