/*
 * shadow_constants.h - Shared constants for Shadow Instrument
 *
 * This header defines constants and structures shared between:
 * - schwung_shim.c (the LD_PRELOAD shim)
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

#define SHM_SHADOW_AUDIO    "/schwung-audio"    /* Shadow's mixed output */
#define SHM_SHADOW_MIDI     "/schwung-midi"     /* MIDI to shadow DSP */
#define SHM_SHADOW_UI_MIDI  "/schwung-ui-midi"  /* MIDI to shadow UI */
#define SHM_SHADOW_DISPLAY  "/schwung-display"  /* Shadow display buffer */
#define SHM_SHADOW_CONTROL  "/schwung-control"  /* Control flags/state */
#define SHM_SHADOW_MOVEIN   "/schwung-movein"   /* Move's audio for shadow */
#define SHM_SHADOW_UI       "/schwung-ui"       /* Shadow UI state */
#define SHM_SHADOW_PARAM      "/schwung-param"        /* Shadow param requests */
#define SHM_SHADOW_MIDI_OUT   "/schwung-midi-out"   /* MIDI output from shadow UI */
#define SHM_SHADOW_MIDI_DSP   "/schwung-midi-dsp"   /* MIDI from shadow UI to DSP slots */
#define SHM_SHADOW_MIDI_INJECT "/schwung-midi-inject" /* MIDI inject into Move's MIDI_IN */
#define SHM_SHADOW_EXT_MIDI_REMAP "/schwung-ext-midi-remap" /* Cable-2 channel remap table */
#define SHM_SHADOW_SCREENREADER "/schwung-screenreader" /* Screen reader announcements */
#define SHM_SHADOW_OVERLAY  "/schwung-overlay"  /* Overlay state (sampler/skipback) */
#define SHM_DISPLAY_LIVE    "/schwung-display-live"    /* Live display for remote viewer */
#define SHM_WEB_PARAM_SET   "/schwung-web-param-set"   /* Web UI → shim param set ring */
#define SHM_WEB_PARAM_NOTIFY "/schwung-web-param-notify" /* Shim → web UI param change ring */

/* ============================================================================
 * Audio Constants
 * ============================================================================ */

#define FRAMES_PER_BLOCK 128    /* Audio frames per ioctl block */

/* ============================================================================
 * Buffer Sizes
 * ============================================================================ */

#define MIDI_BUFFER_SIZE    256   /* Hardware mailbox MIDI area: 64 USB-MIDI packets */
/* Hardware MIDI_OUT region is 20 × 4-byte USB-MIDI packets = 80 bytes.
 * The display buffer starts immediately after at offset 80. Writes to
 * MIDI_OUT must be bounded by this to avoid corrupting the display. */
#define HW_MIDI_OUT_SIZE    80
#define DISPLAY_BUFFER_SIZE 1024  /* 128x64 @ 1bpp = 1024 bytes */
#define CONTROL_BUFFER_SIZE 72  /* bumped for sampler_source_request + sampler_silent (PR #61); leaves headroom in reserved[] */
#define SHADOW_UI_BUFFER_SIZE     512
#define SHADOW_PARAM_BUFFER_SIZE  65664  /* Large buffer for complex ui_hierarchy */
#define SHADOW_MIDI_OUT_BUFFER_SIZE 512  /* MIDI out buffer from shadow UI (128 packets) */
#define SHADOW_MIDI_DSP_BUFFER_SIZE 512  /* MIDI to DSP buffer from shadow UI (128 packets) */
#define SHADOW_MIDI_INJECT_BUFFER_SIZE 256    /* MIDI inject buffer (64 packets) */
#define SHADOW_SCREENREADER_BUFFER_SIZE 8448  /* Screen reader message buffer */
#define SHADOW_OVERLAY_BUFFER_SIZE 256        /* Overlay state buffer */

/* Web UI ring buffer sizes */
#define WEB_PARAM_KEY_LEN     64
#define WEB_PARAM_VALUE_LEN   256    /* Most values are short; hierarchy uses the old channel */
#define WEB_PARAM_SET_ENTRIES 32     /* Max pending set requests */
#define WEB_PARAM_NOTIFY_ENTRIES 64  /* Max pending change notifications */

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
#define SHADOW_UI_FLAG_SET_CHANGED 0x20           /* Set changed - reload slot state */
#define SHADOW_UI_FLAG_JUMP_TO_SETTINGS 0x40     /* Jump to Global Settings */
#define SHADOW_UI_FLAG_JUMP_TO_TOOLS 0x80        /* Jump to Tools menu */

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
    volatile uint8_t restart_move;    /* Signal shim to restart Move (0=no, 1=restart) */
    volatile uint8_t tts_enabled;     /* Screen Reader on/off (1=on, 0=off) */
    volatile uint8_t tts_volume;      /* TTS volume (0-100) */
    volatile uint16_t tts_pitch;      /* TTS pitch in Hz (80-180) */
    volatile float tts_speed;         /* TTS speed multiplier (0.5-6.0) */
    volatile uint8_t overlay_knobs_mode; /* 0=shift, 1=jog_touch, 2=off, 3=native */
    volatile uint8_t display_mirror;     /* 0=off, 1=on (stream display to browser) */
    volatile uint8_t tts_engine;         /* 0=espeak-ng, 1=flite */
    volatile uint8_t pin_challenge_active; /* 0=none, 1=challenge detected, 2=submitted */
    volatile uint8_t display_overlay;     /* 0=off, 1=rect overlay on native, 2=fullscreen */
    volatile uint8_t overlay_rect_x;      /* Overlay rect left edge (pixels, 0-127) */
    volatile uint8_t overlay_rect_y;      /* Overlay rect top edge (pixels, 0-63) */
    volatile uint8_t overlay_rect_w;      /* Overlay rect width (pixels) */
    volatile uint8_t overlay_rect_h;      /* Overlay rect height (pixels) */
    volatile uint16_t tts_debounce_ms;   /* Screen reader debounce in ms (0-1000, default 300) */
    volatile uint8_t set_pages_enabled;  /* 0=off, 1=on (Shift+Vol+Left/Right page switching) */
    volatile uint8_t skip_led_clear;     /* 1=don't clear LEDs on overtake entry, restore snapshot instead */
    volatile uint8_t move_ui_mode;       /* Move's UI mode: 0=unknown, 1=session, 2=note, 3=set_overview */
    volatile uint8_t sampler_cmd;        /* 0=none, 1=start (path in file), 2=stop */
    volatile uint8_t sampler_state_val;  /* Mirrors sampler_state_t: 0=idle,1=armed,2=recording,3=preroll */
    volatile uint8_t mute_move_audio;   /* 1=zero Move's audio output (for silent clip switching) */
    volatile uint8_t sampler_ext_stop;  /* 1=sampler ignores MIDI Stop, only explicit stop works */
    volatile uint8_t wake_slots;       /* 1=clear all slot idle flags (auto-clears after read) */
    volatile uint8_t skipback_require_volume; /* 0=Shift+Capture, 1=Shift+Vol+Capture */
    volatile uint8_t preview_cmd;          /* 0=none, 1=play (path in file), 2=stop */
    volatile uint8_t pad_block;            /* 1=suppress pad notes (68-99) from reaching Move */
    volatile uint8_t suspend_overtake;  /* 1=suspend (skip exit hook), 0=normal exit */
    volatile uint8_t open_tool_cmd;     /* 0=none, 1=open tool (path in /data/UserData/schwung/open_tool_cmd.json) */
    volatile uint8_t shadow_ui_trigger; /* Shadow UI trigger mode: 0=long-press only, 1=Shift+Vol only, 2=both */
    volatile uint8_t speaker_active;    /* 1=built-in speaker active (from CC 115 line-out detect) */
    volatile uint8_t line_in_connected; /* 1=line-in cable plugged (from CC 114 mic-in detect); 0=internal mic */
    volatile uint8_t sampler_source_request; /* 0=no request, 1=set Resample, 2=set Move Input. Shim resets to 0 after applying. */
    volatile uint8_t sampler_silent;     /* 1=suppress sampler screen-reader announcements (e.g. "Sample saved") for tool-driven recordings */
    volatile uint16_t skipback_seconds; /* Skipback rolling buffer length: 30/60/120/180/240/300 */
    volatile uint8_t resume_last_tool;  /* 1=JUMP_TO_TOOLS should resume the most-recently-suspended tool instead of opening the menu */
    volatile uint8_t midi_indicator_enabled; /* 1=draw "ccN" MIDI channel indicator while a note is held */
    /* Co-run state: one struct, accessed via helpers below. `target` selects
     * which peer co-runs (chain editor or Move firmware), `id` is its identity
     * (chain slot 0-3 or tool track 0-7; -1 unused when target=NONE), and
     * `keep_mask` is the tool's CORUN_GRP_* input manifest (0 = default split).
     * Never read these directly outside the helpers — `target` may be 0 (NONE)
     * with stale `id` / `keep_mask` from a prior session. */
    volatile struct {
        int8_t target;       /* corun_target_t */
        int8_t id;
        uint16_t keep_mask;
    } corun;
    volatile uint8_t shadow_display_owner; /* display_owner_t: who currently owns the OLED. Independent of shadow_display_mode (which only says "shadow session active"). */
    volatile uint8_t reserved[1];
} shadow_control_t;

/* Co-run control-surface groups. A co-running overtake tool declares which
 * groups it KEEPS (corun_keep_mask); every other group's input cedes to the
 * co-run UI (Schwung's chain editor, or Move firmware). One canonical
 * event->group map (corun_group_for_event) is shared by every routing site so
 * the let-through and suppress-from-tool decisions can never drift apart. */
#define CORUN_GRP_OLED          (1u << 0)
#define CORUN_GRP_PADS          (1u << 1)
#define CORUN_GRP_STEPS         (1u << 2)
#define CORUN_GRP_TRANSPORT     (1u << 3)
#define CORUN_GRP_JOG           (1u << 4)  /* jog turn (CC 14) + jog click (CC 3) */
#define CORUN_GRP_TRACK_BUTTONS (1u << 5)  /* CC 40-43 */
#define CORUN_GRP_KNOBS         (1u << 6)  /* CC 71-78 */
#define CORUN_GRP_MASTER        (1u << 7)  /* CC 79 */
#define CORUN_GRP_SHIFT         (1u << 8)  /* CC 49 */
#define CORUN_GRP_BACK          (1u << 9)  /* CC 51 */
#define CORUN_GRP_MENU          (1u << 10) /* CC 50 — framework exit gesture */
#define CORUN_GRP_TOUCH         (1u << 11) /* capacitive-touch notes 0-9 */

/* Default keep-set: the canonical sequencer-style co-run split — tool keeps
 * pads, step buttons, transport, and Menu; cedes the nav surface + screen
 * to the peer. Used whenever corun_keep_mask == 0. */
#define CORUN_KEEP_DEFAULT (CORUN_GRP_PADS | CORUN_GRP_STEPS | CORUN_GRP_TRANSPORT | CORUN_GRP_MENU)

/* Opt-out flag for tools that prefer their own exit gesture. When this bit is
 * set in keep_mask, the framework does NOT auto-exit on Back press — Back
 * routes per normal keep_mask rules (cedes to peer for sub-view nav unless
 * the tool also keeps CORUN_GRP_BACK). Default (bit unset) reserves Back as
 * the framework exit gesture regardless of keep_mask. Lives in the high half
 * of keep_mask; doesn't collide with any CORUN_GRP_* bit. */
#define CORUN_KEEP_BACK (1u << 15)

/* Map a raw cable-0 MIDI event to its control-surface group, or 0 if it isn't a
 * routable surface control (those always stay with the tool). type is the
 * status nibble (0xB0 CC, 0x90/0x80 note); d1 the data byte. Steps/transport
 * are intentionally unclassified for now (always kept) — they have no settled
 * CC map and no co-run consumer cedes them. */
static inline uint16_t corun_group_for_event(uint8_t type, uint8_t d1) {
    if (type == 0xB0) {
        switch (d1) {
            case 3:  case 14: return CORUN_GRP_JOG;
            case 40: case 41: case 42: case 43: return CORUN_GRP_TRACK_BUTTONS;
            case 49: return CORUN_GRP_SHIFT;
            case 50: return CORUN_GRP_MENU;
            case 51: return CORUN_GRP_BACK;
            case 79: return CORUN_GRP_MASTER;
            default: if (d1 >= 71 && d1 <= 78) return CORUN_GRP_KNOBS;
                     return 0;
        }
    }
    if (type == 0x90 || type == 0x80) {
        if (d1 <= 9) return CORUN_GRP_TOUCH;
        if (d1 >= 68 && d1 <= 99) return CORUN_GRP_PADS;
    }
    return 0;
}

/* Effective keep-mask: callers that set only the target gate leave keep_mask 0,
 * which means "use the default split". */
static inline uint16_t corun_keep_mask_eff(uint16_t keep_mask) {
    return keep_mask ? keep_mask : CORUN_KEEP_DEFAULT;
}

/* Co-run target. Stored in shadow_control->corun.target as int8_t. */
typedef enum {
    CORUN_TARGET_NONE        = 0,
    CORUN_TARGET_CHAIN_EDIT  = 1, /* Schwung chain editor co-runs (id = chain slot 0-3) */
    CORUN_TARGET_MOVE_NATIVE = 2, /* Move firmware co-runs (id = tool track 0-7) */
} corun_target_t;

/* OLED ownership during a co-run / shadow session. Stored as uint8_t. */
typedef enum {
    DISPLAY_OWNER_TOOL          = 0, /* the active tool (default, including non-shadow Move mode) */
    DISPLAY_OWNER_SCHWUNG_UI    = 1, /* the schwung shadow UI (overtake menu, chain editor, …) */
    DISPLAY_OWNER_MOVE_FIRMWARE = 2, /* Move firmware (during move_native co-run) */
} display_owner_t;

/* Result of corun_event_owner: which side a given control-surface event belongs
 * to right now. Each call site switches on this once instead of mirroring the
 * `grp && !(keep_mask & grp)` check. */
typedef enum {
    CORUN_OWNER_TOOL = 0, /* event goes to the active tool */
    CORUN_OWNER_PEER,     /* event goes to the co-run UI (chain editor or Move firmware) */
    CORUN_OWNER_BOTH,     /* event reaches both (currently unused; reserved for future shared gestures) */
    CORUN_OWNER_NONE,     /* event is consumed by the framework, reaches neither (e.g. Back exit gesture during co-run) */
} corun_owner_t;

/* Accessors — never touch the corun struct directly. */
static inline int corun_active(const volatile shadow_control_t *ctrl) {
    return ctrl && ctrl->corun.target != CORUN_TARGET_NONE;
}
static inline corun_target_t corun_target(const volatile shadow_control_t *ctrl) {
    return ctrl ? (corun_target_t)ctrl->corun.target : CORUN_TARGET_NONE;
}
static inline int corun_id(const volatile shadow_control_t *ctrl) {
    return ctrl ? (int)(int8_t)ctrl->corun.id : -1;
}
static inline uint16_t corun_keep_mask(const volatile shadow_control_t *ctrl) {
    return ctrl ? ctrl->corun.keep_mask : 0;
}

/* Single source of truth for "who owns this event right now?". Both the sh_midi
 * let-through filter and the forward-to-shadow_ui suppress filter call this and
 * switch on the result. Adding a new corun target = extend this function; no
 * mirror checks anywhere else can drift. */
static inline corun_owner_t corun_event_owner(const volatile shadow_control_t *ctrl, uint8_t type, uint8_t d1) {
    if (!corun_active(ctrl)) return CORUN_OWNER_TOOL;
    uint16_t grp = corun_group_for_event(type, d1);
    /* Back: framework-reserved as the exit gesture by default — the shim's
     * own handler ends the session on press, neither side sees the event.
     * Tools that prefer their own exit gesture set CORUN_KEEP_BACK in
     * keep_mask; Back then routes per normal keep_mask rules (cedes to peer
     * for sub-view nav unless CORUN_GRP_BACK is also kept), and the framework
     * stays out of its way. (Menu remains tool-owned by default via
     * keep_mask.) */
    if (grp == CORUN_GRP_BACK && !(ctrl->corun.keep_mask & CORUN_KEEP_BACK)) {
        return CORUN_OWNER_NONE;
    }
    if (!grp) return CORUN_OWNER_TOOL; /* unclassified events always stay with tool */
    uint16_t keep = corun_keep_mask_eff(ctrl->corun.keep_mask);
    return (keep & grp) ? CORUN_OWNER_TOOL : CORUN_OWNER_PEER;
}

/*
 * UI state structure for slot information.
 * Must fit within SHADOW_UI_BUFFER_SIZE bytes.
 */
typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];      /* 0=all, 1-16=specific channel */
    uint16_t slot_volumes[SHADOW_UI_SLOTS];      /* 0-400 percentage */
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
    volatile uint32_t request_id;    /* Monotonic request ID assigned by shadow UI */
    volatile uint32_t response_id;   /* Request ID this response corresponds to */
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
 * MIDI Inject buffer structure.
 * Used by Shadow UI (or external tools) to inject USB-MIDI packets into
 * Move's MIDI_IN buffer, making Move process them as real hardware events.
 * Packets are 4-byte USB-MIDI format (CIN/cable, status, data1, data2).
 */
typedef struct shadow_midi_inject_t {
    volatile uint8_t write_idx;      /* Writer increments after writing */
    volatile uint8_t ready;          /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    uint8_t buffer[SHADOW_MIDI_INJECT_BUFFER_SIZE];  /* USB-MIDI packets (4 bytes each) */
} shadow_midi_inject_t;

/*
 * Cable-2 (external USB) MIDI channel remap table.
 * Active overtake module writes; shim reads on every SPI frame and
 * rewrites the channel byte of cable-2 MIDI_IN events before Move
 * firmware processes them. Solves the cable-2 echo cascade by
 * remapping in-place rather than re-injecting from JS.
 *
 * Disabled globally whenever any chain slot is configured forward=THRU
 * (MPE passthrough). Reset to all-passthrough by the shim on overtake
 * exit (forced — never trusted to JS, since shadow_ui cleanup may not
 * run on crash).
 */
typedef struct schwung_ext_midi_remap_t {
    volatile uint8_t version;        /* 1 = current contract version */
    volatile uint8_t enabled;        /* 0 = bypass, 1 = active */
    volatile uint8_t remap[16];      /* remap[in_ch] = out_ch (0-indexed).
                                      * 0xFF = passthrough for that channel. */
    uint8_t _reserved[46];           /* reserved for v2 (per-source remap, etc) */
} schwung_ext_midi_remap_t;          /* 64 bytes total */

#define EXT_MIDI_REMAP_PASSTHROUGH 0xFF
#define EXT_MIDI_REMAP_BLOCK       0xFE  /* block note-ons from Move; shadow_ui still sees original */
#define EXT_MIDI_REMAP_VERSION     1

/*
 * Web UI param set ring — web server writes, shim drains each audio block.
 * Fire-and-forget: no response needed. ~3ms latency (one audio block).
 * Uses linear buffer with toggle-ready pattern (same as MIDI inject).
 */
typedef struct web_param_set_entry_t {
    uint8_t slot;
    uint8_t reserved[3];
    char key[WEB_PARAM_KEY_LEN];
    char value[WEB_PARAM_VALUE_LEN];
} web_param_set_entry_t;

typedef struct web_param_set_ring_t {
    volatile uint8_t write_idx;    /* Web server increments after writing */
    volatile uint8_t ready;        /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    web_param_set_entry_t entries[WEB_PARAM_SET_ENTRIES];
} web_param_set_ring_t;

/*
 * Web UI param notify ring — shim writes when params change, web server reads.
 * Pushes changed values to browser via WebSocket without polling.
 */
typedef struct web_param_notify_entry_t {
    uint8_t slot;
    uint8_t reserved[3];
    char key[WEB_PARAM_KEY_LEN];
    char value[WEB_PARAM_VALUE_LEN];
} web_param_notify_entry_t;

typedef struct web_param_notify_ring_t {
    volatile uint8_t write_idx;    /* Shim increments after writing */
    volatile uint8_t ready;        /* Toggle to signal new data */
    volatile uint8_t reserved[2];
    web_param_notify_entry_t entries[WEB_PARAM_NOTIFY_ENTRIES];
} web_param_notify_ring_t;

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

/* ============================================================================
 * Overlay State (sampler/skipback, shared from shim to shadow UI)
 * ============================================================================ */

#define SHADOW_OVERLAY_NONE       0
#define SHADOW_OVERLAY_SAMPLER    1
#define SHADOW_OVERLAY_SKIPBACK   2
#define SHADOW_OVERLAY_SHIFT_KNOB 3
#define SHADOW_OVERLAY_SET_PAGE   4

#define SHADOW_SAMPLER_IDLE       0
#define SHADOW_SAMPLER_ARMED      1
#define SHADOW_SAMPLER_RECORDING  2
#define SHADOW_SAMPLER_PREROLL    3
#define SHADOW_SAMPLER_PAUSED    4

/*
 * Overlay state structure for communication from shim to shadow UI.
 * The shim publishes sampler/skipback state here; JS reads it to render overlays.
 * Must fit within SHADOW_OVERLAY_BUFFER_SIZE bytes.
 */
typedef struct shadow_overlay_state_t {
    volatile uint32_t sequence;             /* Incremented on state change; JS polls cheaply */

    volatile uint8_t  overlay_type;         /* NONE / SAMPLER / SKIPBACK */
    volatile uint8_t  sampler_state;        /* IDLE / ARMED / RECORDING / PREROLL / PAUSED */
    volatile uint8_t  sampler_source;       /* 0=Resample, 1=Move Input */
    volatile uint8_t  sampler_cursor;       /* 0=Source menu, 1=Duration menu */

    volatile uint8_t  sampler_fullscreen;   /* 1 = fullscreen takeover */
    volatile uint8_t  skipback_active;      /* 1 = show toast */
    volatile uint16_t sampler_duration_bars; /* 0=until stop, 1/2/4/8/16 */

    volatile int16_t  sampler_vu_peak;      /* Raw peak (0-32767), updated at audio rate */
    volatile uint16_t sampler_bars_completed;
    volatile uint16_t sampler_target_bars;
    volatile uint16_t sampler_overlay_timeout;  /* Frames left for "saved" msg */
    volatile uint16_t skipback_overlay_timeout; /* Frames left for toast */

    volatile uint32_t sampler_samples_written;
    volatile uint32_t sampler_clock_count;
    volatile uint32_t sampler_target_pulses;
    volatile uint32_t sampler_fallback_blocks;
    volatile uint32_t sampler_fallback_target;
    volatile uint8_t  sampler_clock_received;
    volatile uint8_t  transport_playing;       /* 1 = MIDI Start seen, 0 = MIDI Stop seen */

    /* Shift+knob overlay */
    volatile uint8_t  shift_knob_active;        /* 1 = showing shift+knob overlay */
    volatile uint16_t shift_knob_timeout;       /* Frames remaining */
    char shift_knob_patch[64];                  /* Patch/slot name */
    char shift_knob_param[64];                  /* Parameter name */
    char shift_knob_value[32];                  /* Parameter value */

    /* Set page overlay */
    volatile uint8_t  set_page_active;            /* 1 = showing set page toast */
    volatile uint8_t  set_page_current;           /* Current page (0-7) */
    volatile uint8_t  set_page_total;             /* Total pages (8) */
    volatile uint8_t  set_page_loading;           /* 1 = loading (pre-restart), 0 = loaded */
    volatile uint16_t set_page_timeout;           /* Frames remaining for toast */

    /* Preroll state */
    volatile uint8_t  sampler_preroll_enabled;    /* 0=off, 1=on */
    volatile uint8_t  sampler_preroll_active;     /* 1 = currently in preroll countdown */
    volatile uint16_t sampler_preroll_bars_done;  /* Bars completed in preroll */

    volatile float    sampler_bpm;                /* Project BPM from MIDI clock or fallback */

    /* Pad LED colors (notes 68-99, written by shim from Move's MIDI_OUT cache).
     * Index 0 = note 68 (track 4 pad A), index 31 = note 99 (track 1 pad H). */
    volatile uint8_t  pad_led_colors[32];  /* velocity/color for each pad */
} shadow_overlay_state_t;

/* Compile-time size checks */
typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_param_size_check[(sizeof(shadow_param_t) <= SHADOW_PARAM_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_screenreader_size_check[(sizeof(shadow_screenreader_t) <= SHADOW_SCREENREADER_BUFFER_SIZE) ? 1 : -1];
typedef char shadow_overlay_size_check[(sizeof(shadow_overlay_state_t) == SHADOW_OVERLAY_BUFFER_SIZE) ? 1 : -1];
typedef char schwung_ext_midi_remap_size_check[(sizeof(schwung_ext_midi_remap_t) == 64) ? 1 : -1];

#endif /* SHADOW_CONSTANTS_H */
