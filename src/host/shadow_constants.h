/*
 * shadow_constants.h - Shared constants for Shadow Instrument
 *
 * This header defines constants and structures shared between:
 * - move_anything_shim.c (the LD_PRELOAD shim)
 * - shadow_ui.c (the shadow UI host)
 *
 * Single source of truth to prevent drift between components.
 */

#ifndef SHADOW_CONSTANTS_H
#define SHADOW_CONSTANTS_H

#include <stdint.h>

/* ============================================================================
 * Shared Memory Segment Names
 * ============================================================================ */

#define SHM_SHADOW_AUDIO    "/move-shadow-audio"    /* Shadow's mixed output */
#define SHM_SHADOW_MIDI     "/move-shadow-midi"     /* MIDI to shadow DSP */
#define SHM_SHADOW_UI_MIDI  "/move-shadow-ui-midi"  /* MIDI to shadow UI */
#define SHM_SHADOW_DISPLAY  "/move-shadow-display"  /* Shadow display buffer */
#define SHM_SHADOW_CONTROL  "/move-shadow-control"  /* Control flags/state */
#define SHM_SHADOW_MOVEIN   "/move-shadow-movein"   /* Move's audio for shadow */
#define SHM_SHADOW_UI       "/move-shadow-ui"       /* Shadow UI state */
#define SHM_SHADOW_PARAM      "/move-shadow-param"        /* Shadow param requests */
#define SHM_SHADOW_MIDI_OUT   "/move-shadow-midi-out"   /* MIDI output from shadow UI */
#define SHM_SHADOW_MIDI_DSP   "/move-shadow-midi-dsp"   /* MIDI from shadow UI to DSP slots */
#define SHM_SHADOW_SCREENREADER "/move-shadow-screenreader" /* Screen reader announcements */
#define SHM_DISPLAY_LIVE    "/move-display-live"    /* Live display for remote viewer */

/* ============================================================================
 * Buffer Sizes
 * ============================================================================ */

#define MIDI_BUFFER_SIZE    256   /* Hardware mailbox MIDI area: 64 USB-MIDI packets */
#define DISPLAY_BUFFER_SIZE 1024  /* 128x64 @ 1bpp = 1024 bytes */
#define CONTROL_BUFFER_SIZE 64
#define SHADOW_UI_BUFFER_SIZE     512
#define SHADOW_PARAM_BUFFER_SIZE  65664  /* Large buffer for complex ui_hierarchy */
#define SHADOW_MIDI_OUT_BUFFER_SIZE 512  /* MIDI out buffer from shadow UI (128 packets) */
#define SHADOW_MIDI_DSP_BUFFER_SIZE 512  /* MIDI to DSP buffer from shadow UI (128 packets) */
#define SHADOW_SCREENREADER_BUFFER_SIZE 8448  /* Screen reader message buffer */

/* ============================================================================
 * Slot Configuration
 * ============================================================================ */

#define SHADOW_CHAIN_INSTANCES 4
#define SHADOW_UI_SLOTS 4
#define SHADOW_UI_NAME_LEN 64
#define SHADOW_PARAM_KEY_LEN 64
#define SHADOW_PARAM_VALUE_LEN 65536  /* 64KB for large ui_hierarchy and state */
#define SHADOW_SCREENREADER_TEXT_LEN 8192  /* Max text length for screen reader messages */

/* ============================================================================
 * UI Flags (set in shadow_control_t.ui_flags)
 * ============================================================================ */

#define SHADOW_UI_FLAG_JUMP_TO_SLOT 0x01      /* Jump to slot settings on open */
#define SHADOW_UI_FLAG_JUMP_TO_MASTER_FX 0x02 /* Jump to Master FX on open */
#define SHADOW_UI_FLAG_JUMP_TO_OVERTAKE 0x04  /* Jump to overtake module menu */
#define SHADOW_UI_FLAG_SAVE_STATE 0x08        /* Save all state (shutdown imminent) */
#define SHADOW_UI_FLAG_JUMP_TO_SCREENREADER 0x10 /* Jump to screen reader settings */

/* ============================================================================
 * Special Values
 * ============================================================================ */

#define SHADOW_PATCH_INDEX_NONE 65535

/* ============================================================================
 * Shared Structures
 * ============================================================================ */

/*
 * Control structure for communication between shim and shadow UI.
 * Must be exactly SHADOW_CONTROL_BUFFER_SIZE bytes.
 */
typedef struct shadow_control_t {
    volatile uint8_t display_mode;    /* 0=normal, 1=shadow */
    volatile uint8_t shadow_ready;    /* Shadow UI is ready */
    volatile uint8_t should_exit;     /* Signal shadow UI to exit */
    volatile uint8_t midi_ready;      /* New MIDI available (toggle) */
    volatile uint8_t write_idx;       /* MIDI write index */
    volatile uint8_t read_idx;        /* MIDI read index */
    volatile uint8_t ui_slot;         /* UI-highlighted slot for knob routing */
    volatile uint8_t ui_flags;        /* UI flags (SHADOW_UI_FLAG_*) */
    volatile uint16_t ui_patch_index; /* Requested patch index */
    volatile uint16_t reserved16;
    volatile uint32_t ui_request_id;  /* Incremented on patch request */
    volatile uint32_t shim_counter;   /* Debug: shim tick counter */
    volatile uint8_t selected_slot;   /* Track-selected slot (0-3) for playback/knobs */
    volatile uint8_t shift_held;      /* Is shift button currently held? */
    volatile uint8_t overtake_mode;   /* 0=normal, 1=menu (UI events only), 2=module (all events) */
    volatile uint8_t tts_enabled;     /* Screen Reader on/off (1=on, 0=off) */
    volatile uint8_t tts_volume;      /* TTS volume (0-100) */
    volatile uint16_t tts_pitch;      /* TTS pitch in Hz (80-180) */
    volatile float tts_speed;         /* TTS speed multiplier (0.5-6.0) */
    volatile uint8_t overlay_knobs_mode; /* 0=shift, 1=jog_touch, 2=off */
    volatile uint8_t display_mirror;     /* 0=off, 1=on (stream display to browser) */
    volatile uint8_t tts_engine;         /* 0=espeak-ng, 1=flite */
    volatile uint8_t pin_challenge_active; /* 0=none, 1=challenge detected, 2=submitted */
    volatile uint8_t reserved[28];
} shadow_control_t;

/*
 * UI state structure for slot information.
 * Must fit within SHADOW_UI_BUFFER_SIZE bytes.
 */
typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];      /* 0=all, 1-16=specific channel */
    uint8_t slot_volumes[SHADOW_UI_SLOTS];       /* 0-100 percentage */
    int8_t slot_forward_ch[SHADOW_UI_SLOTS];     /* -2=passthrough, -1=auto, 0-15=channel */
    char slot_names[SHADOW_UI_SLOTS][SHADOW_UI_NAME_LEN];
} shadow_ui_state_t;

/*
 * Parameter request structure for get/set operations.
 * Must fit within SHADOW_PARAM_BUFFER_SIZE bytes.
 */
typedef struct shadow_param_t {
    volatile uint8_t request_type;   /* 0=none, 1=set, 2=get */
    volatile uint8_t slot;           /* Which chain slot (0-3) */
    volatile uint8_t response_ready; /* Set by shim when response is ready */
    volatile uint8_t error;          /* Non-zero on error */
    volatile int32_t result_len;     /* Length of result, -1 on error */
    char key[SHADOW_PARAM_KEY_LEN];
    char value[SHADOW_PARAM_VALUE_LEN];
} shadow_param_t;

/*
 * MIDI output structure for shadow UI to send MIDI to hardware.
 * Used by overtake modules (M8, MIDI Controller, etc.) to send MIDI
 * to external USB devices (cable 2) or control Move LEDs (cable 0).
 */
typedef struct shadow_midi_out_t {
    volatile uint8_t write_idx;      /* Shadow UI increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    uint8_t buffer[SHADOW_MIDI_OUT_BUFFER_SIZE];  /* USB-MIDI packets (4 bytes each) */
} shadow_midi_out_t;

/*
 * MIDI-to-DSP structure for shadow UI to send MIDI to chain DSP slots.
 * Used by overtake modules to route MIDI to sound generators/effects.
 * Messages are raw 3-byte MIDI (status, data1, data2), stored 4-byte aligned.
 */
typedef struct shadow_midi_dsp_t {
    volatile uint8_t write_idx;      /* Shadow UI increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    uint8_t buffer[SHADOW_MIDI_DSP_BUFFER_SIZE];  /* Raw MIDI (4 bytes each: status, d1, d2, pad) */
} shadow_midi_dsp_t;

/*
 * Screen reader message structure.
 * Supports both D-Bus announcements and on-device TTS.
 * Shadow UI writes text and updates fields, shim reads and processes.
 */
typedef struct shadow_screenreader_t {
    volatile uint32_t sequence;      /* Incremented for each new message (TTS) */
    volatile uint32_t timestamp_ms;  /* Timestamp of message (for rate limiting) */
    char text[SHADOW_SCREENREADER_TEXT_LEN];
} shadow_screenreader_t;

/* Compile-time size checks */
typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_param_size_check[(sizeof(shadow_param_t) <= SHADOW_PARAM_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_screenreader_size_check[(sizeof(shadow_screenreader_t) <= SHADOW_SCREENREADER_BUFFER_SIZE) ? 1 : -1];

#endif /* SHADOW_CONSTANTS_H */
