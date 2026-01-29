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
#define SHM_SHADOW_PARAM    "/move-shadow-param"    /* Shadow param requests */
#define SHM_SHADOW_MIDI_OUT "/move-shadow-midi-out" /* MIDI output from shadow UI */

/* ============================================================================
 * Buffer Sizes
 * ============================================================================ */

#define MIDI_BUFFER_SIZE    1024  /* 256 USB-MIDI packets for sysex support */
#define DISPLAY_BUFFER_SIZE 1024  /* 128x64 @ 1bpp = 1024 bytes */
#define CONTROL_BUFFER_SIZE 64
#define SHADOW_UI_BUFFER_SIZE     512
#define SHADOW_PARAM_BUFFER_SIZE  65664  /* Large buffer for complex ui_hierarchy */
#define SHADOW_MIDI_OUT_BUFFER_SIZE 1024  /* MIDI out buffer from shadow UI (256 packets) */

/* ============================================================================
 * Slot Configuration
 * ============================================================================ */

#define SHADOW_CHAIN_INSTANCES 4
#define SHADOW_UI_SLOTS 4
#define SHADOW_UI_NAME_LEN 64
#define SHADOW_PARAM_KEY_LEN 64
#define SHADOW_PARAM_VALUE_LEN 65536  /* 64KB for large ui_hierarchy and state */

/* ============================================================================
 * UI Flags (set in shadow_control_t.ui_flags)
 * ============================================================================ */

#define SHADOW_UI_FLAG_JUMP_TO_SLOT 0x01      /* Jump to slot settings on open */
#define SHADOW_UI_FLAG_JUMP_TO_MASTER_FX 0x02 /* Jump to Master FX on open */
#define SHADOW_UI_FLAG_JUMP_TO_OVERTAKE 0x04  /* Jump to overtake module menu */

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
    volatile uint8_t reserved[41];
} shadow_control_t;

/*
 * UI state structure for slot information.
 * Must fit within SHADOW_UI_BUFFER_SIZE bytes.
 */
typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];
    uint8_t slot_volumes[SHADOW_UI_SLOTS];       /* 0-100 percentage */
    int8_t slot_forward_ch[SHADOW_UI_SLOTS];     /* -1=none, 0-15=channel */
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

/* Compile-time size checks */
typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_param_size_check[(sizeof(shadow_param_t) <= SHADOW_PARAM_BUFFER_SIZE) ? 1 : -1];

#endif /* SHADOW_CONSTANTS_H */
