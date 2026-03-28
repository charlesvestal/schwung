/* shadow_chain_types.h - Shared chain slot and capture rule types
 * Extracted from schwung_shim.c so multiple modules can use them. */

#ifndef SHADOW_CHAIN_TYPES_H
#define SHADOW_CHAIN_TYPES_H

#include <stdint.h>

/* Capture rules: bitmaps for which notes/CCs a slot captures */
typedef struct shadow_capture_rules_t {
    uint8_t notes[16];   /* bitmap: 128 notes, 16 bytes */
    uint8_t ccs[16];     /* bitmap: 128 CCs, 16 bytes */
} shadow_capture_rules_t;

/* Fade envelope for seamless patch transitions */
typedef struct slot_fade_t {
    float gain;            /* current gain 0.0-1.0 */
    float target;          /* target gain: 0.0 (fading out) or 1.0 (fading in) */
    float step;            /* per-sample gain change (1.0/FADE_SAMPLES) */
    int pending_patch;     /* patch index to load after fade-out (-1 = none) */
    uint8_t pending_clear; /* tear down DSP after fade-out completes */
} slot_fade_t;

/* 50ms fade at 44100Hz */
#define SLOT_FADE_SAMPLES 2205
#define SLOT_FADE_STEP (1.0f / SLOT_FADE_SAMPLES)

typedef struct shadow_chain_slot_t {
    void *instance;
    int channel;
    int patch_index;
    int active;
    float volume;           /* 0.0 to 1.0, user-set level (never modified by mute/solo) */
    int muted;              /* 1 = muted (Mute+Track or Move speakerOn sync) */
    int soloed;             /* 1 = soloed (Shift+Mute+Track or Move solo-cue sync) */
    int forward_channel;    /* -2 = passthrough, -1 = auto, 0-15 = forward MIDI to this channel */
    char patch_name[64];
    shadow_capture_rules_t capture;  /* MIDI controls this slot captures when focused */
    slot_fade_t fade;                /* fade envelope for seamless transitions */
} shadow_chain_slot_t;

#endif /* SHADOW_CHAIN_TYPES_H */
