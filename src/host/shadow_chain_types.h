/* shadow_chain_types.h - Shared chain slot and capture rule types
 * Extracted from move_anything_shim.c so multiple modules can use them. */

#ifndef SHADOW_CHAIN_TYPES_H
#define SHADOW_CHAIN_TYPES_H

#include <stdint.h>

/* Capture rules: bitmaps for which notes/CCs a slot captures */
typedef struct shadow_capture_rules_t {
    uint8_t notes[16];   /* bitmap: 128 notes, 16 bytes */
    uint8_t ccs[16];     /* bitmap: 128 CCs, 16 bytes */
} shadow_capture_rules_t;

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
} shadow_chain_slot_t;

#endif /* SHADOW_CHAIN_TYPES_H */
